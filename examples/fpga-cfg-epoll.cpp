/*
 * epoll example for FPGA config interface handles for
 * FPP/SPI/CvP and Partial Reconfiguration (PR).
 *
 * This file is released under the GPL-v2 or later.
 *
 * g++ -Wall -o fpga-cfg-epoll fpga-cfg-epoll.cpp
 *
 * Wait for FPP/SPI/CvP or PR configuration:
 * ./fpga-cfg-epoll
 *
 * Example Output:
 *
 *   Waiting for FPGA config event (Ctrl-C to interrupt)...
 *
 *   FPP/SPI or CvP configuration done
 */

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <iostream>

int main(int argc, char** argv)
{
	struct epoll_event ev[2];
	struct epoll_event events[2];
	int epfd, fd_status, fd_ready;
	char buf;
	int n, ret;

	fd_status = open("/sys/kernel/debug/fpga_cfg/fpp_single.0/status", O_RDONLY | O_NONBLOCK);
	if (fd_status < 0) {
		perror("'status' fd open() failed");
		exit(1);
	}

	fd_ready = open("/sys/kernel/debug/fpga_cfg/fpp_single.0/ready", O_RDONLY | O_NONBLOCK);
	if (fd_ready < 0) {
		perror("'ready' fd open() failed");
		close(fd_status);
		exit(1);
	}

	/*
	 * First dummy read to consume data so that the next poll()/epoll_wait()
	 * will wait. Without it first epoll_wait() will return even if no FPGA
	 * configuration has been done yet.
	 */
	pread(fd_status, &buf, 1, 0);
	pread(fd_ready, &buf, 1, 0);

	epfd = epoll_create(2);
	if (epfd < 0) {
		perror("epoll_create() failed");
		exit(1);
	}

	ev[0].data.fd = fd_status;
	ev[0].events = EPOLLPRI | EPOLLERR;

	n = epoll_ctl(epfd, EPOLL_CTL_ADD, fd_status, &ev[0]);
	if (n < 0) {
		perror("'status' epoll_ctl() failed");
		exit(1);
	}

	ev[1].data.fd = fd_ready;
	ev[1].events = EPOLLPRI | EPOLLERR;

	n = epoll_ctl(epfd, EPOLL_CTL_ADD, fd_ready, &ev[1]);
	if (n < 0) {
		perror("'ready' epoll_ctl() failed");
		exit(1);
	}

	while (1) {
		std::cout << std::endl;
		std::cout << "Waiting for FPGA config event (Ctrl-C to interrupt)..." << std::endl;

		n = epoll_wait(epfd, events, 2, -1);
		if (n < 0) {
			perror("epoll_wait() failed");
			exit(1);
		}

		std::cout << std::endl;
		for (int i = 0; i < n; i++) {
			/*
			 * read from offset 0 to consume data, so that
			 * next poll()/epoll_wait() will wait again.
			 */
			buf = 0;
			ret = pread(events[i].data.fd, &buf, 1, 0);
			if (ret < 0) {
				if (events[i].data.fd == fd_status)
					perror("'status' pread() failed");
				else
					perror("'ready' pread() failed");
				if (i == (n - 1))
					exit(1); /* last descriptor failed -> exit */

				/* more descriptors to read */
				continue;
			}
			if (buf == '1') {
				if (events[i].data.fd == fd_status)
					std::cout << "FPP/SPI or CvP configuration done" << std::endl;
				else
					std::cout << "PR configuration done" << std::endl;
			} else {
				if (events[i].data.fd == fd_status)
					std::cerr << "FPP/SPI/CvP configuration error..." << std::endl;
				else
					std::cerr << "PR configuration error..." << std::endl;
			}
		}
	}

	close(fd_ready);
	close(fd_status);
	close(epfd);
	return 0;
}
