CC      := gcc
WINDRES := windres
CFLAGS  := -Wall -Wextra -Wno-missing-field-initializers -O2 -std=c11 -Iinclude -MMD -MP
LDFLAGS := -mwindows -lgdiplus -ldwmapi -luxtheme -lpowrprof -ladvapi32 -luser32 -lgdi32 \
           -lshell32 -lcomctl32 -lpsapi -lkernel32

SRC_DIR   := src
RES_DIR   := resources
BUILD_DIR := build
TARGET    := $(BUILD_DIR)/gameopt.exe

SOURCES := $(wildcard $(SRC_DIR)/*.c)
OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SOURCES))
RES_OBJ := $(BUILD_DIR)/app.res.o

# MinGW's spec file always links its own default-manifest.o, which collides with the
# application manifest in app.rc ("multiple non-default manifests"). Putting an empty object
# of that name on the -B search path shadows it so ours is the only one.
STUB_MANIFEST := $(BUILD_DIR)/default-manifest.o

# windres preprocesses the .rc with "gcc" unless told otherwise. Pointing it at $(CC) keeps the
# whole build on one compiler, e.g. `mingw32-make CC=x86_64-w64-mingw32-gcc`.
RC_PREPROCESS := --preprocessor=$(CC) --preprocessor-arg=-E --preprocessor-arg=-xc \
                 --preprocessor-arg=-DRC_INVOKED

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS) $(RES_OBJ) $(STUB_MANIFEST)
	$(CC) -B$(BUILD_DIR)/ $(OBJECTS) $(RES_OBJ) -o $@ $(LDFLAGS)

$(STUB_MANIFEST): | $(BUILD_DIR)
	echo "" > $(BUILD_DIR)/empty_manifest_stub.c
	$(CC) -c $(BUILD_DIR)/empty_manifest_stub.c -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(RES_OBJ): $(RES_DIR)/app.rc $(RES_DIR)/app.manifest $(wildcard $(RES_DIR)/*.ico) | $(BUILD_DIR)
	$(WINDRES) $(RC_PREPROCESS) -I$(RES_DIR) $< -O coff -o $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: all
	$(TARGET)

clean:
	rm -f $(BUILD_DIR)/*.o $(BUILD_DIR)/*.d $(TARGET)

-include $(OBJECTS:.o=.d)
