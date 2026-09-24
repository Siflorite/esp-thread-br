#pragma once
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
int mock_setsockopt(int, int, int, const void *, socklen_t);
int mock_shutdown(int, int);
#define setsockopt mock_setsockopt
#define shutdown mock_shutdown
ssize_t mock_recv(int, void *, size_t, int);
#define recv mock_recv
