# Host unit tests: firmware C files built with gcc against small shims of the ESP-IDF headers (shim/) and a scripted
# HTTP client (fake.c), with AddressSanitizer. Linux, WSL or macOS (Apple clang, docs/MACOS.md):   make -C tests/host
# IDF_PATH: an ESP-IDF checkout (cJSON, for tests of JSON parsers); on the Windows PC through WSL it is
# /mnt/c/Espressif/esp-idf. test_presence needs it (CI downloads cJSON 1.7.19 and passes CJSON=).
IDF_PATH ?= /mnt/c/Espressif/esp-idf
CJSON := $(IDF_PATH)/components/json/cJSON
FORGE := ../../components/forge_core
LVGLC := ../../components/forge_lvgl
MAIN := ../../main
OUT := build
CFLAGS := -std=gnu11 -D_DEFAULT_SOURCE -g -O1 -Wall -Wno-unused-function -fsanitize=address,undefined \
          -fno-sanitize-recover=undefined -Ishim -I$(FORGE)/include -I$(MAIN) -include shim/host.h
# test_testcon only once the firmware has the pure command table (testcon_registry.c)
TESTS := utf8 version png i18n textfit pager presence map mapcache dlgate slideslots $(if $(wildcard $(FORGE)/testcon_registry.c),testcon)

all: $(TESTS:%=$(OUT)/test_%)
	@set -e; for t in $(TESTS); do ./$(OUT)/test_$$t; done

$(OUT)/test_utf8: test_utf8.c $(FORGE)/include/utf8.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_utf8.c
$(OUT)/test_version: test_version.c $(FORGE)/version.c $(FORGE)/include/version.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_version.c $(FORGE)/version.c
# i18n.c is pure C (its NVS half, i18n_nvs.c, is not compiled here); the texts are the app's
$(OUT)/test_i18n: test_i18n.c $(FORGE)/i18n.c $(FORGE)/include/forge_i18n.h $(MAIN)/i18n_strings.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_i18n.c $(FORGE)/i18n.c
$(OUT)/test_textfit: test_textfit.c $(FORGE)/textfit.c $(FORGE)/include/textfit.h $(MAIN)/montserrat.ttf | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_textfit.c $(FORGE)/textfit.c
# pager.c against a fake of the LVGL calls it makes (lvfake/lvgl.h, implemented in test_pager.c)
$(OUT)/test_pager: test_pager.c $(LVGLC)/pager.c $(LVGLC)/include/pager.h lvfake/lvgl.h | $(OUT)
	$(CC) -Ilvfake $(CFLAGS) -I$(LVGLC)/include -o $@ test_pager.c $(LVGLC)/pager.c
# slide.c's slot choice (slide_slots.c, pure C; slide.c itself needs the panel and LVGL)
$(OUT)/test_slideslots: test_slide_slots.c $(LVGLC)/slide_slots.c $(LVGLC)/include/slide_slots.h | $(OUT)
	$(CC) $(CFLAGS) -I$(LVGLC)/include -o $@ test_slide_slots.c $(LVGLC)/slide_slots.c
# forge_presence's pure half (presence.c, the task and NVS, is not compiled here); cJSON from ESP-IDF
PRES := ../../components/forge_presence
$(OUT)/test_presence: test_presence.c $(PRES)/presence_sm.c $(PRES)/presence_json.c $(PRES)/include/presence.h | $(OUT)
	$(CC) $(CFLAGS) -I$(PRES)/include -I$(PRES) -I$(CJSON) -o $@ test_presence.c $(PRES)/presence_sm.c 	    $(PRES)/presence_json.c $(CJSON)/cJSON.c -lm
$(OUT)/test_testcon: test_testcon.c $(FORGE)/testcon_registry.c $(FORGE)/include/testcon.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ test_testcon.c $(FORGE)/testcon_registry.c
# forge_net's download slot: the bookkeeping (dl_gate.c); the waiting is net.c's and not compiled here
NET := ../../components/forge_net
$(OUT)/test_dlgate: test_dlgate.c $(NET)/dl_gate.c $(NET)/include/dl_gate.h | $(OUT)
	$(CC) $(CFLAGS) -I$(NET)/include -o $@ test_dlgate.c $(NET)/dl_gate.c

# miniz 3.0.2 (MIT) for test_png: its inflate is the ROM's tinfl, its deflate builds the test images. Downloaded once.
MINIZ := $(OUT)/miniz
$(MINIZ)/miniz.c: | $(OUT)
	mkdir -p $(MINIZ)
	curl -fsSL -o $(OUT)/miniz.zip https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip
	python3 -c "import zipfile; zipfile.ZipFile('$(OUT)/miniz.zip').extractall('$(MINIZ)', ['miniz.c', 'miniz.h'])"
$(OUT)/test_png: test_png.c osm_tile.h $(FORGE)/png_rows.c $(FORGE)/include/png_rows.h $(MINIZ)/miniz.c | $(OUT)
	$(CC) $(CFLAGS) -I$(MINIZ) -o $@ test_png.c $(FORGE)/png_rows.c $(MINIZ)/miniz.c
# forge_map's pure half and its drawing (map.c, the task, is not compiled here): geo.c, map_slots.c, map_draw.c with
# png_rows.c, the scripted HTTP client (fake.c) and svc.h's declarations (the test stubs what it calls)
FMAP := ../../components/forge_map
$(OUT)/test_map: test_map.c osm_tile.h fake.c fake.h $(FMAP)/geo.c $(FMAP)/map_slots.c $(FMAP)/map_draw.c $(FMAP)/map_path.c \
                 $(FMAP)/map_priv.h $(FMAP)/include/forge_map.h $(FMAP)/include/forge_geo.h $(FORGE)/png_rows.c \
                 $(MINIZ)/miniz.c | $(OUT)
	$(CC) $(CFLAGS) -I$(MINIZ) -I$(FMAP) -I$(FMAP)/include -I../../components/forge_net/include -o $@ test_map.c fake.c \
	    $(FMAP)/geo.c $(FMAP)/map_slots.c $(FMAP)/map_draw.c $(FMAP)/map_path.c $(FORGE)/png_rows.c $(MINIZ)/miniz.c -lm
# forge_map's flash cache over a RAM-backed I/O table (map_cache.c; the esp_partition binding is not compiled here)
$(OUT)/test_mapcache: test_map_cache.c $(FMAP)/map_cache.c $(FMAP)/include/forge_map.h | $(OUT)
	$(CC) $(CFLAGS) -I$(FMAP) -I$(FMAP)/include -o $@ test_map_cache.c $(FMAP)/map_cache.c -lm

$(OUT):
	mkdir -p $(OUT)
clean:
	rm -rf $(OUT)
.PHONY: all clean
