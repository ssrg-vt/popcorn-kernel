/* A syscall number map from syscall in x86_64 to arm64 */
[20] = 66,	// writev
[1] = 64,   // write
[2] = 56,	// openat
[43] = 202,	// accept
[288] = 242,	// accept4
//[] = 213,	// epoll_create
[291] = 20,	// epoll_create1
[3] = 57,	// close
[72] = 25,	// fcntl
[233] = 21,	// epoll_ctl
[54] = 208,	// setsockopt
[55] = 209,	// getsockopt
[231] = 94,	// exit_group
[40] = 71,	// sendfile
[0] = 63,	// read
[281] = 22,	// epoll_pwait
