#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <xwiimote.h>

// Delay in microseconds between frames.
#define DEADZONE 20

#define WITHIN(value, min, max) (value < min ? min : (value > max) ? max : value)
#define CORRECT_DEADZONE(value) (value < DEADZONE && value > -DEADZONE ? 0 : (value < 0 ? value + DEADZONE : value - DEADZONE))

struct dev_state {
	int8_t btn[XWII_KEY_NUM];
	int rel[4];
};

struct report {
	uint8_t data[5];
};

int open_wiimote(struct xwii_iface **dev) {
	struct xwii_monitor *monitor;
	char *dev_name;
	int res;
	int ret;

	monitor = xwii_monitor_new(true, true);
	if (monitor == NULL) {
		fprintf(stderr, "Error creating monitor\n");
		ret = -0x20;
		goto exit;
	}
	
	dev_name = xwii_monitor_poll(monitor);
	if (dev_name == NULL) {
		fprintf(stderr, "Couldn't find wiimotes\n");
		ret = -0x21;
		goto exit_monitor;
	}

	printf("Opening Wiimote at %s\n", dev_name);
	res = xwii_iface_new(dev, dev_name);
	if (res < 0) {
		fprintf(stderr, "Error creating wiimote: %s\n", strerror(-res));
		ret = -0x22;
		goto exit_dev_name;
	}

	ret = 0;

exit_dev_name:
	free(dev_name);
exit_monitor:
	xwii_monitor_unref(monitor);
exit:
	return ret;
}

int open_iface(struct xwii_iface *dev, unsigned int supported) {
	// Open Nunchuk
	unsigned int iface_available;
	unsigned int iface_opened;
	int i;
	int res;
	int ret;

	iface_available = xwii_iface_available(dev);
	printf("Ifaces available: 0x%08x\n", iface_available);

	// Open either the Classic Controller or Wiimote+Nunchuk
	res = xwii_iface_open(dev, iface_available & supported);
	if (res < 0) {
		fprintf(stderr, "Error opening Wiimote ifaces: %s\n", strerror(-res));
		ret = 0x25;
		goto exit;
	}

	ret = 0;

exit:
	return ret;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr, "Usage: ./use-wiimote <connection directory> [delay millis]\n");
		return 0x01;
	}

	struct report report;
	memset(&report, 0, sizeof(report));
	report.data[0] = 0xA1;
	report.data[1] = 2;

	unsigned int supported = XWII_IFACE_CORE | XWII_IFACE_NUNCHUK | XWII_IFACE_CLASSIC_CONTROLLER;
	int delay_millis = 30;
	double sensitivity = 5e-2;
	double r_factor = 3;
	double nunchuck_correction = 3e-1;

	if (argc >= 3) {
		delay_millis = atoi(argv[2]);
	}
	
	struct timeval delay_val = {
		.tv_sec = 0,
		.tv_usec = delay_millis * 1e3,
	};

	int sock;
	struct sockaddr_un addr;
	struct xwii_iface *dev;
	unsigned int iface_opened;
	struct pollfd pfd;
	struct xwii_event ev;
	bool send_report;
	struct dev_state wii_state, mouse_state;
	struct timeval last_report, prev, now, diff;
	int diff_micro;
	int res;
	int ret;

	// Only open the interrupt path
	char sock_addr_path[108];
	strcpy(sock_addr_path, argv[1]);
	strcat(sock_addr_path, "/interrupt");

	sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (sock == -1) {
		fprintf(stderr, "Unable to create socket: %s\n", strerror(errno));
		ret = 0x10;
		goto err;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX,
	strncpy(addr.sun_path, sock_addr_path, sizeof(addr.sun_path) - 1);
	if (connect(sock, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		fprintf(stderr, "Unable to bind socket: %s\n", strerror(errno));
		ret = 0x11;
		goto err_sock;
	}

	// Connect to a wiimote
	// https://github.com/xwiimote/xwiimote/blob/master/lib/xwiimote.h
	res = open_wiimote(&dev);
	if (res < 0) {
		ret = -res;
		goto err_sock;
	}

	res = open_iface(dev, supported);
	if (res < 0) {
		ret = -res;
		goto err_dev;
	}

	iface_opened = xwii_iface_opened(dev);
	printf("Ifaces opened: 0x%08x\n", iface_opened);
	/*if ((iface_opened & iface_requested) != iface_requested) {
		fprintf(stderr, "Not all Wiimote ifaces opened.\n");
		ret = 0x26;
		goto err_dev;
	}*/

	res = xwii_iface_watch(dev, true);
	if (res < 0) {
		fprintf(stderr, "Error watching iface: %s", strerror(-res));
		ret = 0x24;
		goto err_dev;
	}

	// Reset state
	memset(&wii_state, 0, sizeof(wii_state));
	memset(&mouse_state, 0, sizeof(mouse_state));
	// Reset timers
	if (gettimeofday(&now, NULL) == -1) {
		ret = 0x32;
		fprintf(stderr, "Unable to reset time: %s", strerror(errno));
		goto err_dev;
	}
	memcpy(&last_report, &now, sizeof(now));
	memcpy(&prev, &now, sizeof(now));


	printf("Reading events.\n");
	memset(&pfd, 0, sizeof(pfd));
	pfd.fd = xwii_iface_get_fd(dev);
	pfd.events = POLLIN;

	for (;;) {
		// Poll with timeout
		if (poll(&pfd, 1, delay_millis) == -1) {
			if (errno == EINTR) {
				goto done;
			}
			fprintf(stderr, "Error polling wiimote: %s", strerror(errno));
			ret = 0x80;
		}

		send_report = false;

		res = xwii_iface_dispatch(dev, &ev, sizeof(ev));
		if (res < 0) {
			// EAGAIN means no events are available.
			if (res == -EAGAIN) {
				goto skip_event;
			}
			fprintf(stderr, "Error reading events from wiimote: %s", strerror(-res));
			0x31;
			goto err_dev;
		}

		switch (ev.type) {
			case XWII_EVENT_GONE:
			goto done;
			// On WATCH, reopen ifaces.
			case XWII_EVENT_WATCH:
			open_iface(dev, supported);
			break;
			case XWII_EVENT_KEY:
			case XWII_EVENT_CLASSIC_CONTROLLER_KEY:
			wii_state.btn[ev.v.key.code] = ev.v.key.state ? 1 : 0;
			// Send a report, but not on repeats.
			if (ev.v.key.state != 2) {
				send_report = true;
			}
			break;
			case XWII_EVENT_CLASSIC_CONTROLLER_MOVE:
			wii_state.rel[0] = CORRECT_DEADZONE(ev.v.abs[0].x);
			wii_state.rel[1] = CORRECT_DEADZONE(ev.v.abs[0].y);
			// If right stick is moved
			wii_state.rel[2] = CORRECT_DEADZONE(ev.v.abs[1].x);
			wii_state.rel[3] = CORRECT_DEADZONE(ev.v.abs[1].y);
			break;
			case XWII_EVENT_NUNCHUK_MOVE:
			// Process nunchuck movement and also process left stick.
			wii_state.rel[0] = CORRECT_DEADZONE(ev.v.abs[0].x) * nunchuck_correction;
			wii_state.rel[1] = CORRECT_DEADZONE(ev.v.abs[0].y) * nunchuck_correction;
			break;
		}

skip_event:

		// Get time since last event
		if (gettimeofday(&now, NULL) == -1) {
			ret = 0x82;
			fprintf(stderr, "Unable to get time: %s", strerror(errno));
			goto err_dev;
		}

		// Force a report if the timer has expired.
		timersub(&now, &last_report, &diff);
		if (timercmp(&diff, &delay_val, >)) {
			send_report = true;
		}

		// Calculate time difference from last frame.
		timersub(&now, &prev, &diff);
		diff_micro = diff.tv_sec * 1e6 + diff.tv_usec;

		// Move rel based on time difference
		mouse_state.rel[0] += (wii_state.rel[0] + r_factor * wii_state.rel[2]) * diff_micro;
		mouse_state.rel[1] += (wii_state.rel[1] + r_factor * wii_state.rel[3]) * diff_micro;

		// Copy button state
		mouse_state.btn[0] = wii_state.btn[XWII_KEY_A] || wii_state.btn[XWII_KEY_TR];
		mouse_state.btn[1] = wii_state.btn[XWII_KEY_B];
		mouse_state.btn[2] = 0;
		printf("Btn: %d\n", mouse_state.btn[0]);

		memcpy(&prev, &now, sizeof(now));


		if (!send_report) {
			continue;
		}

		// Create report.
		printf("Sending report\n");

		// Calculate movement
		printf("Rel x = %d, y = %d\n", mouse_state.rel[0], mouse_state.rel[1]);
		int rel_x = mouse_state.rel[0] / 1e3 * sensitivity;
		int rel_y = -mouse_state.rel[1] / 1e3 * sensitivity;
		printf("To x = %d, y = %d\n", rel_x, rel_y);

		// Write report fields
		report.data[2] = mouse_state.btn[0] | (mouse_state.btn[1] << 1) | (mouse_state.btn[2] << 2);
		report.data[3] = WITHIN(rel_x, -128, 127);
		report.data[4] = WITHIN(rel_y, -128, 127);

		// Send the message
		if (send(sock, &report, sizeof(report), 0) == -1) {
			fprintf(stderr, "Unable to send report: %s", strerror(errno));
			ret = 0x083;
			goto err_dev;
		}

		// Reset rel
		mouse_state.rel[0] = 0;
		mouse_state.rel[1] = 0;

		// Reset report timer
		memcpy(&last_report, &now, sizeof(now));
	}

done:
	ret = 0;

err_dev:
	xwii_iface_unref(dev);
err_sock:
	close(sock);
err:
	return ret;
}