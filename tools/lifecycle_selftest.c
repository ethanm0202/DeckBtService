/* Exercise the real service lifecycle and loopback listener with a failed controller.
 * No service registration, UART access, firmware, or device attachment. */
#define DECKBT_VERSION "lifecycle-selftest"
#define QcaBackendStart FixtureBackendStart
#define wmain DeckbtProgramMain
#include "../src/service/deckbt_usbip.c"
#undef wmain
#undef QcaBackendStart
#include <shellapi.h>

static volatile LONG s_Attempts;
static LONG s_NotifyAttempt;
static HANDLE s_AttemptEvent;
static BOOL s_CtsFailure;
static BOOL s_InterruptCtsSequence;
static int s_Failures;
static QCA_BACKEND s_Backend;
static USBIP_DEVICE s_Device;

#define CHECK(Condition, Message) do { \
    if (Condition) { printf("  ok   %s\n", Message); } \
    else { printf("  FAIL %s\n", Message); s_Failures++; } \
} while (0)

DWORD FixtureBackendStart(QCA_BACKEND *Backend, CRITICAL_SECTION *Lock, HCI_TRANSPORT *Transport)
{
    LONG attempt = InterlockedIncrement(&s_Attempts);
    (void)Lock;
    (void)Transport;
    Backend->CtsUnresponsive = (BOOLEAN)(s_CtsFailure && !(s_InterruptCtsSequence && attempt == 2));
    if (attempt == s_NotifyAttempt) {
        (void)SetEvent(s_AttemptEvent);
    }
    return Backend->CtsUnresponsive ? ERROR_NOT_READY : ERROR_FILE_NOT_FOUND;
}

static void FixtureReport(DWORD State, DWORD WaitHintMs)
{
    (void)State;
    (void)WaitHintMs;
}

static DWORD WINAPI FixtureRun(LPVOID Context)
{
    (void)Context;
    return Run(FixtureReport);
}

static void ResetFixture(BOOL CtsFailure, BOOL InterruptSequence, LONG NotifyAttempt)
{
    ZeroMemory(&s_Backend, sizeof(s_Backend));
    ZeroMemory(&g_Transport, sizeof(g_Transport));
    InterlockedExchange(&s_Attempts, 0);
    InterlockedExchange(&g_Stop, 0);
    s_CtsFailure = CtsFailure;
    s_InterruptCtsSequence = InterruptSequence;
    s_NotifyAttempt = NotifyAttempt;
    g_StartAttempts = 20;
    g_Ready = 0;
    g_Running = 0;
    g_Suspended = 0;
    (void)ResetEvent(s_AttemptEvent);
    (void)ResetEvent(g_StopEvent);
    (void)ResetEvent(g_ResumeEvent);
    (void)ResetEvent(g_SuspendPending);
}

/* The blocked service stays reachable but refuses to attach a non-working radio. */
static BOOL ImportUnavailable(void)
{
    static const unsigned char request[40] = { 0x01, 0x11, 0x80, 0x03, 0, 0, 0, 0, '1', '-', '1' };
    static const char expected[] = { 0x01, 0x11, 0, 0x03, 0, 0, 0, 1 };
    struct sockaddr_in address;
    int size = sizeof(address);
    char reply[sizeof(expected)];
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    DWORD timeout = 3000;
    int received = 0;
    BOOL ok = FALSE;

    if (client == INVALID_SOCKET) {
        return FALSE;
    }
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    if (getsockname(g_Listener, (struct sockaddr *)&address, &size) == 0 &&
        connect(client, (struct sockaddr *)&address, size) == 0 &&
        send(client, (const char *)request, (int)sizeof(request), 0) == (int)sizeof(request)) {
        while (received < (int)sizeof(reply)) {
            int n = recv(client, reply + received, (int)sizeof(reply) - received, 0);
            if (n <= 0) {
                break;
            }
            received += n;
        }
        ok = received == (int)sizeof(reply) && memcmp(reply, expected, sizeof(reply)) == 0;
    }
    closesocket(client);
    return ok;
}

static void ScenarioBlocked(BOOL InterruptSequence)
{
    HANDLE thread;
    DWORD exitCode = STILL_ACTIVE;
    LONG expectedAttempts = InterruptSequence ? 5 : 3;

    ResetFixture(TRUE, InterruptSequence, expectedAttempts);
    thread = CreateThread(NULL, 0, FixtureRun, NULL, 0, NULL);
    CHECK(thread != NULL, "start service lifecycle without touching hardware");
    if (thread == NULL) {
        return;
    }
    CHECK(WaitForSingleObject(s_AttemptEvent, 20000) == WAIT_OBJECT_0,
          "reach three consecutive physical wake failures");
    CHECK(WaitForSingleObject(thread, 3500) == WAIT_TIMEOUT && s_Attempts == expectedAttempts,
          "no exit for SCM restart and no fourth consecutive CTS retry");
    CHECK(ImportUnavailable(), "live loopback server refuses to attach the unavailable radio");
    (void)SetEvent(g_ResumeEvent);
    CHECK(WaitForSingleObject(thread, 100) == WAIT_TIMEOUT && s_Attempts == expectedAttempts,
          "unrelated resume signal does not restart the failed controller");
    (void)SetEvent(g_StopEvent);
    CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0,
          "operator stop remains responsive while recovery is blocked");
    (void)GetExitCodeThread(thread, &exitCode);
    CHECK(exitCode == ERROR_SUCCESS, "intentional stop does not trigger SCM failure recovery");
    CloseHandle(thread);
}

static void TestServiceCommandLineQuoting(void)
{
    WCHAR *cmdLine = NULL;
    const wchar_t *testArgv[] = {
        L"--firmware-dir",
        L"C:\\Program Files\\QCA\\",
        L"--usbip",
        L"C:\\Program Files\\USBip\\usbip.exe",
        L"--stop-file",
        L"C:\\Users\\李明\\Bluetooth Firmware\\stop.txt",
        L"simple",
        L"with space",
        L"tab\tinside",
        L"embedded\"quote",
        L"trailing\\backslash\\",
        L""
    };
    int parsedArgc = 0;
    LPWSTR *parsed = NULL;

    printf("\nservice command-line quoting, dynamic sizing, and round-trip through CommandLineToArgvW\n");
    cmdLine = BuildServiceCommandLine(L"C:\\Program Files\\DeckBtService\\deckbt-usbip.exe",
                                      (int)ARRAYSIZE(testArgv), (wchar_t **)testArgv);
    CHECK(cmdLine != NULL, "build service command line with spaces, Unicode and trailing backslashes");

    if (cmdLine != NULL) {
        /* Assert that trailing backslash before closing quote is escaped as \\" */
        CHECK(wcsstr(cmdLine, L"\"C:\\Program Files\\QCA\\\\\"") != NULL,
              "trailing backslash before closing quote is escaped as \\\\\"");

        /* Assert Unicode path was preserved without ANSI code-page mangling */
        CHECK(wcsstr(cmdLine, L"\"C:\\Users\\李明\\Bluetooth Firmware\\stop.txt\"") != NULL,
              "Unicode path with CJK characters preserved in command line");

        parsed = CommandLineToArgvW(cmdLine, &parsedArgc);
        CHECK(parsed != NULL, "CommandLineToArgvW parses constructed command line");
        if (parsed != NULL) {
            CHECK(parsedArgc == (int)(ARRAYSIZE(testArgv) + 2), "parsed argument count matches expected");
            if (parsedArgc == (int)(ARRAYSIZE(testArgv) + 2)) {
                CHECK(wcscmp(parsed[0], L"C:\\Program Files\\DeckBtService\\deckbt-usbip.exe") == 0,
                      "exe path round-trips correctly");
                CHECK(wcscmp(parsed[1], L"--service") == 0,
                      "--service flag present as second argument");
                CHECK(wcscmp(parsed[2], L"--firmware-dir") == 0,
                      "--firmware-dir argument intact");
                CHECK(wcscmp(parsed[3], L"C:\\Program Files\\QCA\\") == 0,
                      "quoted path with trailing backslash round-trips exactly without swallowing next argument");
                CHECK(wcscmp(parsed[4], L"--usbip") == 0,
                      "--usbip argument intact and not swallowed");
                CHECK(wcscmp(parsed[5], L"C:\\Program Files\\USBip\\usbip.exe") == 0,
                      "--usbip path with space round-trips correctly");
                CHECK(wcscmp(parsed[6], L"--stop-file") == 0,
                      "--stop-file argument intact");
                CHECK(wcscmp(parsed[7], L"C:\\Users\\李明\\Bluetooth Firmware\\stop.txt") == 0,
                      "Unicode path with CJK characters round-trips byte-for-byte");
                CHECK(wcscmp(parsed[8], L"simple") == 0,
                      "unquoted argument intact");
                CHECK(wcscmp(parsed[9], L"with space") == 0,
                      "space-containing argument intact");
                CHECK(wcscmp(parsed[10], L"tab\tinside") == 0,
                      "tab-containing argument intact");
                CHECK(wcscmp(parsed[11], L"embedded\"quote") == 0,
                      "embedded quote intact");
                CHECK(wcscmp(parsed[12], L"trailing\\backslash\\") == 0,
                      "unquoted trailing backslash intact");
                CHECK(wcscmp(parsed[13], L"") == 0,
                      "empty argument intact");
            }
            LocalFree(parsed);
        }
        free(cmdLine);
    }

    /* Bug 1 regression: Verify that Exe without spaces does NOT underallocate by 2 characters */
    {
        WCHAR *noSpaceExeCmd = BuildServiceCommandLine(L"C:\\DeckBtService\\deckbt-usbip.exe",
                                                      (int)ARRAYSIZE(testArgv), (wchar_t **)testArgv);
        CHECK(noSpaceExeCmd != NULL, "build service command line with Exe path containing no spaces (no 2-char underallocation)");
        if (noSpaceExeCmd != NULL) {
            const wchar_t *expectedPrefix = L"\"C:\\DeckBtService\\deckbt-usbip.exe\" --service";
            CHECK(wcsncmp(noSpaceExeCmd, expectedPrefix, wcslen(expectedPrefix)) == 0,
                  "unconditionally quoted Exe prefix formatted correctly");
            free(noSpaceExeCmd);
        }
    }

    /* Bug 2 regression: Verify that SCM limit supports up to 32,767 characters and rejects beyond */
    {
        wchar_t *hugeArgv[350];
        wchar_t hugePath[120];
        for (int k = 0; k < 100; k++) {
            hugePath[k] = L'A';
        }
        hugePath[100] = L'\\';
        hugePath[101] = L'\0';
        for (int i = 0; i < 350; i++) {
            hugeArgv[i] = hugePath;
        }
        /* 300 args of ~105 chars is ~31,500 chars (valid in SCM limit <= 32,767) */
        WCHAR *largeCmd = BuildServiceCommandLine(L"C:\\DeckBtService\\deckbt-usbip.exe", 300, hugeArgv);
        CHECK(largeCmd != NULL, "BuildServiceCommandLine accepts valid large command lines (>8192 chars) up to 32,767 characters");
        if (largeCmd != NULL) {
            CHECK(wcslen(largeCmd) > 8192 && wcslen(largeCmd) <= 32767,
                  "large command line size confirmed between 8192 and 32,767 characters");
            free(largeCmd);
        }

        /* 350 args of ~105 chars is ~36,750 chars (exceeds 32,767) */
        WCHAR *tooBigCmd = BuildServiceCommandLine(L"C:\\DeckBtService\\deckbt-usbip.exe", 350, hugeArgv);
        CHECK(tooBigCmd == NULL, "BuildServiceCommandLine rejects command lines exceeding 32,767 characters without truncating");
        if (tooBigCmd != NULL) {
            free(tooBigCmd);
        }
    }
}

int main(void)
{
    DWORD result;
    HANDLE thread;
    DWORD exitCode = STILL_ACTIVE;

    QueryPerformanceFrequency(&g_QpcFrequency);
    QueryPerformanceCounter(&g_QpcStart);
    InitializeCriticalSection(&g_Lock);
    g_Device = &s_Device;
    g_Qca = &s_Backend;
    g_UseUart = 1;
    g_NoAttach = 1;
    g_AllowUserImport = 1;   /* exercise readiness refusal, not the separate PID security check */
    g_Port = 0;   /* isolated ephemeral loopback port */
    wcscpy_s(g_FirmwareDir, ARRAYSIZE(g_FirmwareDir), L"fixture-no-firmware-access");
    g_Kick = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_ResumeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_SuspendPending = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_AttemptEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_Kick || !g_StopEvent || !g_ResumeEvent || !g_SuspendPending || !s_AttemptEvent) {
        return 1;
    }

    printf("persistent CTS failure pauses recovery, not the service control path\n");
    ScenarioBlocked(FALSE);
    printf("\nan intervening ordinary startup error resets the CTS-failure sequence\n");
    ScenarioBlocked(TRUE);

    printf("\nordinary firmware/start errors retain their normal failure status\n");
    ResetFixture(FALSE, FALSE, 2);
    g_StartAttempts = 2;
    result = Run(FixtureReport);
    CHECK(result == ERROR_FILE_NOT_FOUND && s_Attempts == 2,
          "ordinary start failure exits with the real error after configured attempts");

    printf("\nstop interrupts recovery before its next retry\n");
    ResetFixture(TRUE, FALSE, 1);
    thread = CreateThread(NULL, 0, FixtureRun, NULL, 0, NULL);
    CHECK(thread != NULL, "start cancellable recovery");
    if (thread != NULL) {
        CHECK(WaitForSingleObject(s_AttemptEvent, 3000) == WAIT_OBJECT_0, "first physical wake failed");
        (void)SetEvent(g_StopEvent);
        CHECK(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "stop interrupts the retry delay");
        (void)GetExitCodeThread(thread, &exitCode);
        CHECK(exitCode == ERROR_SUCCESS && s_Attempts == 1, "no retry or failure exit after stop");
        CloseHandle(thread);
    }
    TestServiceCommandLineQuoting();
    CloseHandle(s_AttemptEvent);
    CloseHandle(g_SuspendPending);
    CloseHandle(g_ResumeEvent);
    CloseHandle(g_StopEvent);
    CloseHandle(g_Kick);
    DeleteCriticalSection(&g_Lock);
    printf("\n%s\n", s_Failures ? "LIFECYCLE SELFTEST FAILED" : "LIFECYCLE SELFTEST PASSED");
    return s_Failures ? 1 : 0;
}
