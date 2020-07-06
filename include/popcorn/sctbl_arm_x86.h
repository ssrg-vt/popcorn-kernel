/* A syscall number map from syscall in arm64 to x86_64 */
[66] = 20,	// writev
[64] = 1,      // write
[56] = 2,	// openat
[202] = 43,	// accept
[242] = 288,	// accept4
//[] = 213,	// epoll_create
[20] = 291,	// epoll_create1
[57] = 3,	// close
[25] = 72,	// fcntl
[21] = 233,	// epoll_ctl
[208] = 54,	// setsockopt
[209] = 55,	// getsockopt
[94] = 231,	// exit_group
[71] = 40,	// sendfile
[63] = 0,	// read
[22] = 281,	// epoll_pwait

