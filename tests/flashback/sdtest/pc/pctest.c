/* Runs the Flashback SD test's rounds (main.c's runRound and printResults,
   included as they are) against the simulated card in 3ds.h, on a virtual
   clock, and checks the results add up. */
#define main sdtest_main
#include "../source/main.c"
#undef main

int main(void)
{
    static u8 buf[SLOT];
    g_buf = buf;
    g_fileSize = FILE_SIZE;
    g_videoBase = VIDEO_BASE;
    Round rounds[ROUNDS];
    memset(rounds, 0, sizeof(rounds));
    int errors = 0;
    for (int i = 0; i < ROUNDS; i++)
    {
        g_vt = (u64)i * 1000 * SYSCLOCK_ARM11;     /* each round starts at a whole 10 s */
        if (!runRound(0, &rounds[i], i + 1)) { printf("round %d stopped\n", i + 1); errors++; }
    }

    /* the results, captured to check their width (the top screen is 50 columns) */
    FILE *f = fopen("results.txt", "w");
    printResults(f, rounds, ROUNDS);
    fclose(f);
    f = fopen("results.txt", "r");
    char line[256];
    printf("\n--- results file ---\n");
    while (fgets(line, sizeof(line), f))
    {
        printf("%s", line);
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') n--;
        if (n > 50) { printf("  ^ %zu columns\n", n); errors++; }
    }
    fclose(f);

    for (int i = 0; i < ROUNDS; i++)
    {
        const Round *r = &rounds[i];
        u32 arrived = ROUND_SECONDS * FPS;
        if (r->frames > arrived || arrived - r->frames > r->mostWaiting + 1) { printf("round %d: frames don't add up\n", i + 1); errors++; }
        for (u32 k = 1; k < ROOMS; k++)
            if (r->dropped[k] > r->dropped[k - 1]) { printf("round %d: more room dropped more\n", i + 1); errors++; }
        for (u32 k = 0; k < ROOMS; k++)
            if (r->mostWaiting <= kRooms[k] && r->dropped[k] != 0) { printf("round %d: room %lu dropped with at most %lu waiting\n", i + 1, (unsigned long)kRooms[k], (unsigned long)r->mostWaiting); errors++; }
        if (r->audioWrites < ROUND_SECONDS * 3 || r->audioWrites > ROUND_SECONDS * 4) { printf("round %d: %lu audio writes\n", i + 1, (unsigned long)r->audioWrites); errors++; }
        if (r->capped) { printf("round %d: gave up\n", i + 1); errors++; }
    }
    printf("\nwrites %ld, outside their part of the file %ld\n", g_writes, g_badWrites);
    if (g_badWrites) errors++;
    printf("%s (%d errors)\n", errors ? "FAILED" : "all good", errors);
    return errors != 0;
}
