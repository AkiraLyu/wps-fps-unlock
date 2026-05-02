CC ?= gcc
CFLAGS ?= -O2 -g

BUILD_DIR := build
TARGET := $(BUILD_DIR)/libwps_qelapsed_scale.so
PROBE := $(BUILD_DIR)/qelapsed_probe
STARTUP_PROBE := $(BUILD_DIR)/qelapsed_startup_probe
SRC := src/wps_qelapsed_scale.c
VERSION_SCRIPT := wps-qelapsed-scale.map

.PHONY: all clean verify verify-runtime verify-startup-patch

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(TARGET): $(SRC) $(VERSION_SCRIPT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -Wall -Wextra -shared \
		-Wl,--version-script=$(VERSION_SCRIPT) \
		-o $@ $(SRC) -ldl -pthread

$(PROBE): tools/qelapsed_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -o $@ $< -ldl

$(STARTUP_PROBE): tools/qelapsed_startup_probe.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wall -Wextra -o $@ $< -ldl \
		-Wl,--no-as-needed /usr/lib/office6/libQt5CoreKso.so.5.12.12 \
		-Wl,--as-needed -Wl,-rpath,/usr/lib/office6

verify: $(TARGET)
	readelf -Ws $(TARGET) | grep 'QElapsedTimer.*@@Qt_5'

verify-runtime: $(TARGET) $(PROBE)
	LD_LIBRARY_PATH=/usr/lib/office6 $(PROBE)
	WPS_QELAPSED_SCALE=0.5 WPS_QELAPSED_DEBUG=1 LD_PRELOAD=$(abspath $(TARGET)) LD_LIBRARY_PATH=/usr/lib/office6 $(PROBE)
	WPS_QELAPSED_SCALE=0.5 WPS_QELAPSED_DEBUG=1 LD_PRELOAD=$(abspath $(TARGET)):/usr/lib/faketime/libfaketimeMT.so.1 FAKETIME='+0 x2' LD_LIBRARY_PATH=/usr/lib/office6 $(PROBE)

verify-startup-patch: $(TARGET) $(STARTUP_PROBE)
	LD_LIBRARY_PATH=/usr/lib/office6 $(STARTUP_PROBE)
	WPS_QELAPSED_SCALE=0.5 WPS_QELAPSED_DEBUG=1 LD_PRELOAD=$(abspath $(TARGET)) LD_LIBRARY_PATH=/usr/lib/office6 $(STARTUP_PROBE)

clean:
	rm -rf $(BUILD_DIR)
