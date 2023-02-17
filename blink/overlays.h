#ifndef BLINK_OVERLAYS_H_
#define BLINK_OVERLAYS_H_
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_OVERLAYS ":o"

int OverlaysChdir(const char *);
int SetOverlays(const char *, bool);
char *OverlaysGetcwd(char *, size_t);
int OverlaysUnlink(int, const char *, int);
int OverlaysMkdir(int, const char *, mode_t);
int OverlaysMkfifo(int, const char *, mode_t);
int OverlaysOpen(int, const char *, int, int);
int OverlaysChmod(int, const char *, mode_t, int);
int OverlaysAccess(int, const char *, mode_t, int);
int OverlaysSymlink(const char *, int, const char *);
int OverlaysStat(int, const char *, struct stat *, int);
int OverlaysChown(int, const char *, uid_t, gid_t, int);
int OverlaysRename(int, const char *, int, const char *);
ssize_t OverlaysReadlink(int, const char *, char *, size_t);
int OverlaysLink(int, const char *, int, const char *, int);
int OverlaysUtime(int, const char *, const struct timespec[2], int);

#endif /* BLINK_OVERLAYS_H_ */
