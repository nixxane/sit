# sit version
VERSION = 0.1

# paths
PREFIX = /usr/local

# OS: linux, win (MSYS2 UCRT64), mac
OS = linux

PKG_CONFIG = pkg-config
PKGS = sdl2 mpv libcurl zlib

GLLIBS_linux = -lGL
GLLIBS_win   = -lopengl32
GLLIBS_mac   = -framework OpenGL

# flags
CPPFLAGS = -DVERSION=\"${VERSION}\" -isystem vendor
CFLAGS   = -std=c99 -pedantic -Wall -Wextra -O2 `${PKG_CONFIG} --cflags ${PKGS}` ${CPPFLAGS}
LDFLAGS  = `${PKG_CONFIG} --libs ${PKGS}` ${GLLIBS_${OS}} -lm

# compiler and linker
CC = gcc
