void plr_volume(int vol_l, int vol_r);
void plr_reset(BOOL wait);
void plr_stop();
void plr_pause();
void plr_resume();
int plr_pump();
int plr_play(const char *path, unsigned int from, unsigned int to);
int plr_length(const char *path);
void plr_cleanup(void);
