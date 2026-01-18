//
// Created by ekeleze on 1/17/26.
//

#include <stdio.h>
#include <string.h>
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/gba/core.h>
#include <mgba-util/vfs.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/time.h>

#include "video.h"

static volatile bool running = true;

void signal_handler(int signum) {
	(void)signum;
	running = false;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("Usage: %s <rom_file>\n", argv[0]);
		return 1;
	}

	bool scaled = false;

	if (argc > 2) {
		for (int i = 1; i < argc; i++)
		{
			char *arg = argv[i];

			if (strncmp(arg, "--scaled=", 9) == 0)
			{
				char *value = arg + 9;
				scaled = strcmp(value, "true") == 0;
			}

			if (strncmp(arg, "-s=", 3) == 0)
			{
				char *value = arg + 3;
				scaled = strcmp(value, "true") == 0;
			}
		}
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGHUP, signal_handler);

	printf("Opening ROM file: %s\n", argv[1]);
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		printf("Failed to open ROM file\n");
		return 1;
	}

	struct stat st;
	fstat(fd, &st);
	size_t rom_size = st.st_size;
	printf("ROM size: %zu bytes\n", rom_size);

	void* rom_data = malloc(rom_size);
	read(fd, rom_data, rom_size);
	close(fd);

	printf("Creating GBA core...\n");
	struct mCore* core = GBACoreCreate();
	if (!core) {
		printf("Failed to create core\n");
		free(rom_data);
		return 1;
	}

	mCoreConfigInit(&core->config, NULL);

	core->init(core);

	struct VFile* vf = VFileFromMemory(rom_data, rom_size);
	if (!core->loadROM(core, vf)) {
		printf("Failed to load ROM!\n");
		core->deinit(core);
		free(rom_data);
		return 1;
	}
	printf("ROM loaded\n");

	unsigned width, height;
	core->baseVideoSize(core, &width, &height);
	mColor* videoBuffer = malloc(width * height * sizeof(mColor));
	core->setVideoBuffer(core, videoBuffer, width);

	mCoreLoadConfig(core);

	core->reset(core);

	printf("Initializing video...\n");
	video_init(scaled);
	printf("Video initialized\n");

	struct timeval frame_start, frame_end;

	while (running) {
		const long target_frame_time = 16750;

		gettimeofday(&frame_start, NULL);

		core->runFrame(core);
		video_draw(core);

		gettimeofday(&frame_end, NULL);
		long elapsed = (frame_end.tv_sec - frame_start.tv_sec) * 1000000 +
					   (frame_end.tv_usec - frame_start.tv_usec);

		long sleep_time = target_frame_time - elapsed;
		if (sleep_time > 0) {
			usleep(sleep_time);
		}
	}

	printf("\nShutting down...\n");

	core->deinit(core);
	vf->close(vf);
	free(videoBuffer);
	free(rom_data);

	return 0;
}