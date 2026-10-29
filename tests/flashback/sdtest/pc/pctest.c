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

    /* the sustained test, both ways, on the same card (each from a whole 10 s) */
    static u8 big[SUS_BUF_BYTES];
    static Minute a[SUS_MINUTES], b[SUS_MINUTES];
    g_buf = big;
    g_fileSize = SUS_FILE_SIZE;
    g_videoBase = SUS_VIDEO_BASE;
    g_vt = 10000ULL * SYSCLOCK_ARM11;
    if (!runSustained(0, false, a)) { printf("sustained, as Flashback: stopped\n"); errors++; }
    g_vt = 20000ULL * SYSCLOCK_ARM11;
    if (!runSustained(0, true, b)) { printf("sustained, back to back: stopped\n"); errors++; }
    f = fopen("sustained.txt", "w");
    printSustained(f, a, b, SUS_MINUTES);
    fclose(f);
    f = fopen("sustained.txt", "r");
    printf("\n--- sustained results file ---\n");
    while (fgets(line, sizeof(line), f))
    {
        printf("%s", line);
        size_t n = strlen(line);
        if (n > 0 && line[n - 1] == '\n') n--;
        if (n > 50) { printf("  ^ %zu columns\n", n); errors++; }
    }
    fclose(f);
    for (int way = 0; way < 2; way++)
    {
        const Minute *m = way ? b : a;
        u32 arrived = 0, written = 0, dropped = 0, audio = 0, writes = 0;
        for (u32 i = 0; i < SUS_MINUTES; i++)
        {
            arrived += m[i].arrived; written += m[i].written; dropped += m[i].dropped;
            audio += m[i].audioWrites; writes += m[i].writes;
            /* (the last minute's last few may come after the test's last look) */
            if (m[i].arrived < 60 * FPS - (i + 1 == SUS_MINUTES ? 10 : 0) || m[i].arrived > 60 * FPS + 1) { printf("way %d minute %lu: %lu frames came\n", way, (unsigned long)i + 1, (unsigned long)m[i].arrived); errors++; }
        }
        /* every frame that came was written, dropped, or still waits at the end (in staging or a capture buffer) */
        if (written + dropped > arrived || arrived - written - dropped > SUS_ROOM_BYTES / 102400 + SUS_CAPTURE) { printf("way %d: frames don't add up (%lu came, %lu written, %lu dropped)\n", way, (unsigned long)arrived, (unsigned long)written, (unsigned long)dropped); errors++; }
        if (audio < SUS_MINUTES * 60 * 3 || audio > SUS_MINUTES * 60 * 4) { printf("way %d: %lu audio writes\n", way, (unsigned long)audio); errors++; }
        if (way == 0 && writes != written) { printf("as Flashback: %lu writes for %lu frames\n", (unsigned long)writes, (unsigned long)written); errors++; }
        if (way == 0 && dropped == 0) { printf("as Flashback: nothing dropped (the simulated card's slow stretches should)\n"); errors++; }
    }
    /* the simulated card takes as long for any write, so writing several frames at once must drop fewer */
    u32 dropA = 0, dropB = 0;
    for (u32 i = 0; i < SUS_MINUTES; i++) { dropA += a[i].dropped; dropB += b[i].dropped; }
    if (dropB >= dropA) { printf("back to back dropped %lu, as Flashback %lu\n", (unsigned long)dropB, (unsigned long)dropA); errors++; }
    printf("\nwrites %ld, outside their part of the file %ld\n", g_writes, g_badWrites);
    if (g_badWrites) errors++;
    printf("%s (%d errors)\n", errors ? "FAILED" : "all good", errors);
    return errors != 0;
}
