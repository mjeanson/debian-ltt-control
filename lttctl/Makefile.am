## Process this file with automake to produce Makefile.in

AM_CFLAGS = -DPACKAGE_DATA_DIR=\""$(datadir)"\" -DPACKAGE_BIN_DIR=\""$(bindir)"\"

# Ensure scripts are portable by depending only on /bin/sh
SH = /bin/sh

bin_PROGRAMS = lttctl
bin_SCRIPTS = ltt-armall ltt-disarmall \
  ltt-armtap ltt-disarmtap
CLEANFILES = $(bin_SCRIPTS)
EXTRA_DIST = ltt-armall.sh ltt-disarmall.sh \
  ltt-armtap.sh ltt-disarmtap.sh

ltt-armall: ltt-armall.sh
	rm -f ltt-armall
	echo "#!"$(SH) > ltt-armall
	cat $(srcdir)/ltt-armall.sh >> ltt-armall
	chmod ugo+x ltt-armall

ltt-disarmall: ltt-disarmall.sh
	rm -f ltt-disarmall
	echo "#!"$(SH) > ltt-disarmall
	cat $(srcdir)/ltt-disarmall.sh >> ltt-disarmall
	chmod ugo+x ltt-disarmall

ltt-armtap: ltt-armtap.sh
	rm -f ltt-armtap
	echo "#!"$(SH) > ltt-armtap
	cat $(srcdir)/ltt-armtap.sh >> ltt-armtap
	chmod ugo+x ltt-armtap

ltt-disarmtap: ltt-disarmtap.sh
	rm -f ltt-disarmtap
	echo "#!"$(SH) > ltt-disarmtap
	cat $(srcdir)/ltt-disarmtap.sh >> ltt-disarmtap
	chmod ugo+x ltt-disarmtap

lttctl_SOURCES = \
	lttctl.c
lttctl_DEPENDENCIES = ../liblttctl/liblttctl.la
lttctl_LDADD = $(lttctl_DEPENDENCIES)

