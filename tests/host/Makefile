# Host unit tests: firmware C files built with gcc against small shims of the ESP-IDF headers (shim/), with
# AddressSanitizer and UBSan. Linux or WSL:   make -C tests/host
# espforge's components at the tag in main/idf_component.yml (managed_components after an idf.py build), else
# tools/fetch_forge.py's .espforge (as esp32-s3-rtcquebec). Here: this app's protocol core (mesh_proto.c), the i18n
# core with this app's texts, and the framework tests of the components the app uses (utf8, version, textfit with the
# app's font, pager, the test console's command table). forge_net, forge_ota, forge_map and forge_presence are not
# tested here (not used, or not at this tag).
FORGE ?= $(if $(wildcard ../../managed_components/forge_core/CMakeLists.txt),../../managed_components,../../.espforge/components)
ifeq ($(wildcard $(FORGE)/forge_core/i18n.c),)
$(error no espforge components at $(FORGE): build the firmware once, or python tools/fetch_forge.py, or FORGE=<espforge>/components)
endif
CORE := $(FORGE)/forge_core
# forge_lvgl is a local copy here (components/forge_lvgl, the drag hook): prefer it over the tag
LVGLC := $(if $(wildcard ../../components/forge_lvgl/pager.c),../../components/forge_lvgl,$(FORGE)/forge_lvgl)
MAIN := ../../main
OUT := build
CFLAGS := -std=gnu11 -D_DEFAULT_SOURCE -g -O1 -Wall -Wno-unused-function -fsanitize=address,undefined \
          -fno-sanitize-recover=undefined -Ishim -I$(CORE)/include -I$(MAIN) -include shim/host.h
TESTS := utf8 version textfit pager testcon i18n mesh wave fw_auth rps voice

all: $(TESTS:%=$(OUT)/test_%)
	@set -e; for t in $(TESTS); do ./$(OUT)/test_$$t; done

$(OUT)/test_utf8: test_utf8.c $(CORE)/include/utf8.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_utf8.c
$(OUT)/test_version: test_version.c $(CORE)/version.c $(CORE)/include/version.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_version.c $(CORE)/version.c
$(OUT)/test_textfit: test_textfit.c $(CORE)/textfit.c $(CORE)/include/textfit.h $(MAIN)/montserrat.ttf | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_textfit.c $(CORE)/textfit.c
# pager.c against a fake of the LVGL calls it makes (lvfake/lvgl.h, implemented in test_pager.c)
$(OUT)/test_pager: test_pager.c $(LVGLC)/pager.c $(LVGLC)/include/pager.h lvfake/lvgl.h | $(OUT)
	$(CC) -Ilvfake $(CFLAGS) -I$(LVGLC)/include -o $@ test_pager.c $(LVGLC)/pager.c
$(OUT)/test_testcon: test_testcon.c $(CORE)/testcon_registry.c $(CORE)/include/testcon.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_testcon.c $(CORE)/testcon_registry.c
# i18n.c is pure C (its NVS half, i18n_nvs.c, is not compiled here); the texts are the app's
$(OUT)/test_i18n: test_i18n.c $(CORE)/i18n.c $(CORE)/include/forge_i18n.h $(MAIN)/i18n_strings.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_i18n.c $(CORE)/i18n.c
# mesh_proto.c is pure C99 (no ESP-IDF header, no shim): built strictly, without host.h
$(OUT)/test_mesh: test_mesh.c $(MAIN)/mesh_proto.c $(MAIN)/mesh_proto.h check.h | $(OUT)
	$(CC) -std=c99 -pedantic -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined \
	    -I$(MAIN) -o $@ test_mesh.c $(MAIN)/mesh_proto.c
# wave.c is pure C99 too (libm only for its tables); test_wave prints frame times under the sanitizers, and
# `make bench` builds it optimised without them for a fairer time (still a PC, not the ESP32-S3)
WAVE_SRC := test_wave.c $(MAIN)/wave.c
WAVE_DEP := $(WAVE_SRC) $(MAIN)/wave.h $(MAIN)/mesh_proto.h check.h
$(OUT)/test_wave: $(WAVE_DEP) | $(OUT)
	$(CC) -std=c99 -pedantic -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined 	    -I$(MAIN) -o $@ $(WAVE_SRC) -lm
$(OUT)/bench_wave: $(WAVE_DEP) | $(OUT)
	$(CC) -std=c99 -pedantic -Wall -Wextra -Werror -O2 -I$(MAIN) -o $@ $(WAVE_SRC) -lm
bench: $(OUT)/bench_wave
	./$(OUT)/bench_wave
# fw_auth.c (the signed pull's layouts and checks) is pure C99 as well, with mesh_proto.c for the version compare
FWA_SRC := test_fw_auth.c $(MAIN)/fw_auth.c $(MAIN)/mesh_proto.c
$(OUT)/test_fw_auth: $(FWA_SRC) $(MAIN)/fw_auth.h $(MAIN)/mesh_proto.h check.h | $(OUT)
	$(CC) -std=c99 -pedantic -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined 	    -I$(MAIN) -o $@ $(FWA_SRC)
# rps.c (rock paper scissors rounds) is pure C99
$(OUT)/test_rps: test_rps.c $(MAIN)/rps.c $(MAIN)/rps.h check.h | $(OUT)
	$(CC) -std=c99 -pedantic -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined 	    -I$(MAIN) -o $@ test_rps.c $(MAIN)/rps.c

# The voice clip's pure parts (adpcm.c, clip_chunks.c, ringtone.c), C99 with libm
VOICE_SRC := test_voice.c $(MAIN)/adpcm.c $(MAIN)/clip_chunks.c $(MAIN)/ringtone.c
$(OUT)/test_voice: $(VOICE_SRC) $(MAIN)/adpcm.h $(MAIN)/clip_chunks.h $(MAIN)/ringtone.h check.h | $(OUT)
	$(CC) -std=c99 -pedantic -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -I$(MAIN) -o $@ $(VOICE_SRC) -lm

$(OUT):
	mkdir -p $(OUT)
clean:
	rm -rf $(OUT)
.PHONY: all clean bench

