#include "test_support.h"

namespace altest {
namespace {

int g_failuresInCurrentTest = 0;
int g_totalFailures = 0;

}  // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

Registrar::Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }

void reportFailure(const char* file, int line, const std::string& message) {
    ++g_failuresInCurrentTest;
    ++g_totalFailures;
    std::printf("    %s:%d: %s\n", file, line, message.c_str());
}

int runAll() {
    int failedTests = 0;
    for (const TestCase& test : registry()) {
        g_failuresInCurrentTest = 0;
        std::printf("[ RUN  ] %s\n", test.name);
        test.fn();
        if (g_failuresInCurrentTest == 0) {
            std::printf("[  OK  ] %s\n", test.name);
        } else {
            std::printf("[ FAIL ] %s (%d 件)\n", test.name, g_failuresInCurrentTest);
            ++failedTests;
        }
    }

    std::printf("\n%zu テスト中 %d 失敗 (アサーション失敗 %d 件)\n", registry().size(), failedTests,
                g_totalFailures);
    return failedTests == 0 ? 0 : 1;
}

}  // namespace altest

int main() { return altest::runAll(); }
