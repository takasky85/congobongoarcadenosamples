# Congo Bongo libretro core
#   make                       -> congobongo_libretro.so (Linux) / .dll (Windows) / .dylib (macOS)
#   Android: ndk-build -C jni   (see jni/Android.mk), or the GitHub Actions workflow
TARGET_NAME := congobongo

ifeq ($(platform),)
  platform = unix
  ifeq ($(shell uname -s),)
    platform = win
  else ifneq ($(findstring MINGW,$(shell uname -s)),)
    platform = win
  else ifneq ($(findstring Darwin,$(shell uname -s)),)
    platform = osx
  endif
endif

CXX ?= g++
CXXFLAGS += -O3 -std=gnu++17 -fPIC -Wall -Wno-unused-function -Wno-misleading-indentation
LDFLAGS  += -shared

ifeq ($(platform),win)
  TARGET := $(TARGET_NAME)_libretro.dll
  LDFLAGS += -static-libgcc -static-libstdc++ -Wl,--version-script=link.T
else ifeq ($(platform),osx)
  TARGET := $(TARGET_NAME)_libretro.dylib
  LDFLAGS += -dynamiclib
else
  TARGET := $(TARGET_NAME)_libretro.so
  LDFLAGS += -Wl,--version-script=link.T -Wl,--no-undefined
endif

SOURCES := libretro.cpp
DEPS := ../congobongo.cpp ../z80.c ../z80.h ../congo_perc_synth.h ../congo_gorilla_synth.h libretro.h

all: $(TARGET)

$(TARGET): $(SOURCES) $(DEPS)
	$(CXX) $(CXXFLAGS) $(SOURCES) -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
