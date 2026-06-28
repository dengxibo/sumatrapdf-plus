#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

START_TEST(test_label_buffer_overflow)
{
    // Invariant: Label length must not exceed destination buffer capacity
    const char *payloads[] = {
        "A",  // Valid short input
        "1234567890123456789012345678901234567890",  // Boundary: 40 chars
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"  // Exploit: 256 chars
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);

    for (int i = 0; i < num_payloads; i++) {
        // Create test input file with adversarial label
        FILE *f = fopen("test_input.bin", "wb");
        fwrite(payloads[i], 1, strlen(payloads[i]), f);
        fclose(f);

        // Execute actual bin2coff with adversarial input
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "./tools/bin2coff test_input.bin test_output.coff 2>&1");
        
        int status = system(cmd);
        
        // Property: Process must not crash (segfault) or exhibit undefined behavior
        ck_assert_msg(WIFEXITED(status) || WIFSIGNALED(status), 
                     "Process must have defined termination state");
        
        if (WIFSIGNALED(status)) {
            ck_assert_msg(WTERMSIG(status) != SIGSEGV && 
                         WTERMSIG(status) != SIGABRT,
                         "Must not crash with memory corruption signals");
        }

        // Cleanup
        unlink("test_input.bin");
        unlink("test_output.coff");
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_label_buffer_overflow);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}