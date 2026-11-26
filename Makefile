CC = clang
CFLAGS = -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined

.PHONY: test
test:
	@set -eu; build_dir=$$(mktemp -d "$${TMPDIR:-/tmp}/u6-pcm.XXXXXX"); \
	trap 'rm -rf "$$build_dir"' EXIT; \
	$(CC) $(CFLAGS) -Isrc tests/test_pcm.c src/pcm_service.c -lm -o "$$build_dir/test_pcm"; \
	"$$build_dir/test_pcm"; \
	$(CC) $(CFLAGS) -Isrc tests/test_u6_alsa.c src/pcm_service.c src/u6_alsa.c -lm -o "$$build_dir/test_u6_alsa"; \
	"$$build_dir/test_u6_alsa" "$$build_dir"; \
	$(CC) $(CFLAGS) -Isrc tests/test_u6_board.c src/u6_board.c src/u6_lease.c -o "$$build_dir/test_u6_board"; \
	"$$build_dir/test_u6_board" "$$build_dir"; \
	$(CC) $(CFLAGS) -Isrc tests/test_pcm_session.c src/pcm_session.c src/pcm_service.c -lm -o "$$build_dir/test_pcm_session"; \
	"$$build_dir/test_pcm_session"; \
	$(CC) $(CFLAGS) -Isrc tests/test_u6_raw.c src/pcm_service.c src/u6_alsa.c src/u6_raw.c -lm -o "$$build_dir/test_u6_raw"; \
	"$$build_dir/test_u6_raw" "$$build_dir"; \
	$(CC) $(CFLAGS) -Iandroid -Isrc tests/test_apple_track.c android/apple_track.c -o "$$build_dir/test_apple_track"; \
	"$$build_dir/test_apple_track"; \
	$(CC) $(CFLAGS) -Isrc tests/test_u6_volume.c src/u6_volume.c -lm -o "$$build_dir/test_u6_volume"; \
	"$$build_dir/test_u6_volume"; \
	$(CC) $(CFLAGS) -Isrc tests/test_u6_lease.c src/u6_lease.c -o "$$build_dir/test_u6_lease"; \
	"$$build_dir/test_u6_lease" "$$build_dir"
