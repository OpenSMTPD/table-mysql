noinst_PROGRAMS =	table-mysql

table_mysql_SOURCES =	table_mysql.c dict.c log.c table_stdio.c util.c

LDADD =			$(LIBOBJS)

dist_man5_MANS =	table-mysql.5

smtpdir =		${prefix}/libexec/smtpd

install-exec-local: $(noinst_PROGRAMS)
	$(MKDIR_P) $(DESTDIR)$(smtpdir)
	$(INSTALL_PROGRAM_ENV) $(INSTALL_PROGRAM) $(noinst_PROGRAMS) $(DESTDIR)$(smtpdir)

README.md: table-mysql.5
	mandoc -Tmarkdown -l table-mysql.5 > README.md
