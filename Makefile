# Trinamic stepper motion plugin for FPP.
#
# At the repo root so FPP's compileBinaries() rebuilds the .so whenever FPP
# core is upgraded.
SRCDIR ?= /opt/fpp/src
include $(SRCDIR)/makefiles/common/setup.mk

# setup.mk does not apply the per-platform defines (-DPLATFORM_PI); without
# them the plugin would see a different view of FPP's headers than libfpp.
include $(SRCDIR)/makefiles/platform/*.mk

TARGET = libStepperMotion.so

SRCS = \
	src/TmcDevice.cpp \
	src/TmcAxis.cpp \
	src/AxisConfig.cpp \
	src/AxisWorker.cpp \
	src/TmcManager.cpp \
	src/TmcOutput.cpp \
	src/TmcApi.cpp \
	src/TmcCommands.cpp \
	src/TmcPlugin.cpp

OBJS = $(SRCS:.cpp=.o)
DEPS = $(OBJS:.o=.d)

all: $(TARGET)

%.o: %.cpp
	$(CCACHE) $(CXXCOMPILER) $(CFLAGS) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CCACHE) $(CXXCOMPILER) -shared $(CFLAGS) $(CXXFLAGS) $(OBJS) -o $@ \
		$(LDFLAGS) -L$(SRCDIR) -lfpp

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

.PHONY: all clean

-include $(DEPS)

# Development checks (make test-config), when their folder is present; the
# published plugin builds without them.
-include $(wildcard */test.mk)
