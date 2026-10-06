#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Smoke test: run gardener-dump in both modes against a fixture, check
 * exit code and that stdout contains expected substrings. */
static int run(const char *cmd, const char *must_contain) {
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, p);
    buf[n] = 0;
    int rc = pclose(p);
    if (rc != 0) {
        fprintf(stderr, "FAIL: command failed (rc=%d): %s\n", rc, cmd);
        return -1;
    }
    if (must_contain && !strstr(buf, must_contain)) {
        fprintf(stderr, "FAIL: output did not contain \"%s\"\n%s\n",
                must_contain, buf);
        return -1;
    }
    return 0;
}

int main(void) {
    /* Config-only mode. */
    if (run("../build/gardener-dump --config ../configs/channels.toml",
            "coolant_temp") != 0) return 1;

    /* Feed mode. Write a tiny fixture inline via shell. */
    const char *fixture =
        "printf '%s\\n' "
        "'{\"t\":1000,\"ch\":[{\"id\":1,\"v\":20.5}]}' "
        "'{\"t\":2000,\"ch\":[{\"id\":1,\"v\":21.0}]}' "
        "> /tmp/gardener_dump_test.jsonl";
    if (system(fixture) != 0) return 1;

    if (run("../build/gardener-dump --feed /tmp/gardener_dump_test.jsonl "
            "--channels ../configs/channels.toml --counts --quiet",
            "coolant_temp") != 0) return 1;

    remove("/tmp/gardener_dump_test.jsonl");
    printf("test_dump_cli: all checks passed\n");
    return 0;
}