# Project Name
TARGET = HillsideSlicer

# Library Locations - adjust these to your local DaisyExamples paths
LIBDAISY_DIR ?= ../../libDaisy
DAISYSP_DIR ?= ../../DaisySP

# Sources
CPP_SOURCES = HillsideSlicer.cpp

# Core
OPT = -O2

# Includes
C_INCLUDES += -I.

# Set the board target
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core

include $(SYSTEM_FILES_DIR)/Makefile
