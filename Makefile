CXX= g++
LD = g++

SRCDIR  = ./src
BINDIR  = ./bin
TMPDIR  = ./tmp
INCDIR =  $(DCONLINE_PATH)/DCOV/EBuilder/include
INC =  -I$(DCONLINE_PATH)/DCOV/EBuilder/include
INC += -I$(DCONLINE_PATH)/DCDOGSifier

ROOTCFLAGS   := $(shell root-config --cflags)
ROOTLDFLAGS  := $(shell root-config --ldflags)
ROOTLIBS     := $(shell root-config --libs)

CXXFLAGS      = -O2 -Wall -fPIC $(INC) -I/usr/include/mysql -I/usr/include/mysql++
LDFLAGS       = $(INC) # does nothing?
SOFLAGS       = -shared

CXXFLAGS     += $(ROOTCFLAGS)
LDFLAGS      += $(ROOTLDFLAGS)
LIBS          = $(ROOTLIBS)
LIBS         += -L/usr/lib/mysql -lmysqlclient -lmysqlpp
MAIN=EventBuilder.cxx
TARGET=$(MAIN:%.cxx=$(BINDIR)/%)

all: dir $(TARGET)
#------------------------------------------------------------------------------

USBSTREAMO        = $(TMPDIR)/USBstream.o
USBSTREAMSO       = $(TMPDIR)/libUSBstream.so
USBSTREAMLIB      = $(shell pwd)/$(USBSTREAMSO)

EVENTBUILDERO    = $(TMPDIR)/EventBuilder.o
EVENTBUILDERS    = $(SRCDIR)/EventBuilder.cxx

OBJS          = $(USBSTREAMO) $(EVENTBUILDERO)

#------------------------------------------------------------------------------

.SUFFIXES: .cxx .o .so

all: dir $(TARGET)

$(USBSTREAMSO):     $(USBSTREAMO)
	$(LD) $(SOFLAGS) $(LDFLAGS) $^ -o  $@
	@echo "$@ done"

$(TARGET): $(USBSTREAMSO) $(EVENTBUILDERO)
	$(LD) $(LDFLAGS) $(EVENTBUILDERO) $(USBSTREAMLIB) $(LIBS) -o $@
	@echo "$@ done"

clean:
	@rm -rf $(BINDIR) $(TMPDIR) core $(SRCDIR)/*Dict*

$(TMPDIR)/%.o: $(SRCDIR)/%.cxx $(INCDIR)/USBstream.h $(INCDIR)/USBstream-TypeDef.h $(INCDIR)/USBstreamUtils.h
	$(CXX)  $(CXXFLAGS) -c $< -o $@

dir:
	@echo '<< Creating OV EBuilder directory structure >>'
	@mkdir -p $(BINDIR) $(TMPDIR)
	# Why?
	@touch /var/tmp/OV_EBuilder.txt
	@echo '<< Creating OV EBuilder directory structure succeeded >>'
