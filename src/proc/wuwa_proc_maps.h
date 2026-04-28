#ifndef ANDROID_WUWA_WUWA_PROC_MAPS_H
#define ANDROID_WUWA_WUWA_PROC_MAPS_H

#include <linux/net.h>

int do_get_proc_maps(struct socket* sock, void __user* arg);

#endif // ANDROID_WUWA_WUWA_PROC_MAPS_H
