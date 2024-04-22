TABLE\_MYSQL(5) - File Formats Manual

# NAME

**table\_mysql** - format description for smtpd MySQL or MariaDB tables

# DESCRIPTION

This manual page documents the file format of MySQL or MariaDB tables used
by the
smtpd(8)
mail daemon.

The format described here applies to tables as defined in
smtpd.conf(5).

# MYSQL TABLE

A mysql table allows the storing of usernames, passwords, aliases, and domains
in a format that is shareable across various machines that support
mysql(1).

The table is used by
smtpd(8)
when authenticating a user, when user information such as user-id and/or
home directory is required for a delivery, when a domain lookup may be required,
and/or when looking for an alias.

A MySQL table consists of one or more
mysql(1)
databases with one or more tables.

If the table is used for authentication, the password should be
encrypted using the
crypt(3)
function. Such passwords can be generated using the
encrypt(1)
utility or
smtpctl(8)
encrypt command.

# MYSQL TABLE CONFIG FILE

The following configuration options are available:

**host**
*hostname*

> This is the host running MySQL or MariaDB.
> For example:

> > host db.example.com

**username**
*username*

> The username required to talk to the MySQL or MariaDB database.
> For example:

> > username maildba

**password**
*password*

> The password required to talk to the MySQL or MariaDB database.
> For example:

> > password OpenSMTPDRules!

**database**
*databse*

> The name of the MySQL or MariaDB database.
> For example:

> > databse opensmtpdb

**query\_alias**
*SQL statement*

> This is used to provide a query to look up aliases. The question mark
> is replaced with the appropriate data. For alias it is the left hand side of
> the SMTP address. This expects one VARCHAR to be returned with the user name
> the alias resolves to.

**query\_credentials**
*SQL statement*

> This is used to provide a query for looking up user credentials. The question
> mark is replaced with the appropriate data. For credentials it is the left
> hand side of the SMTP address. The query expects that there are two VARCHARS
> returned, one with a user name and one with a password in
> crypt(3)
> format.

**query\_domain**
*SQL statement*

> This is used to provide a query for looking up a domain. The question mark
> is replaced with the appropriate data. For the domain it would be the
> right hand side of the SMTP address. This expects one VARCHAR to be returned
> with a matching domain name.

**query\_mailaddrmap**
*SQL statement*

> This is used to provide a query to look up senders. The question mark
> is replaced with the appropriate data. This expects one VARCHAR to be
> returned with the address the sender is allowed to send mails from.

A generic SQL statement would be something like:

	query_ SELECT value FROM table WHERE key=?;

# EXAMPLES

## GENERIC EXAMPLE

Example based on the OpenSMTPD FAQ: Building a Mail Server
The filtering part is excluded in this example.

The configuration below is for a medium-size mail server which handles
multiple domains with multiple virtual users and is based on several
assumptions. One is that a single system user named vmail is used for all
virtual users. This user needs to be created:

	# useradd -g =uid -c "Virtual Mail" -d /var/vmail -s /sbin/nologin vmail
	# mkdir /var/vmail
	# chown vmail:vmail /var/vmail

*MySQL schema*

	CREATE TABLE domains (
	  id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
	  domain VARCHAR(255) NOT NULL DEFAULT ''
	);
	CREATE TABLE virtuals (
	    id INT AUTO_INCREMENT PRIMARY KEY,
	    email VARCHAR(255) NOT NULL DEFAULT '',
	    destination VARCHAR(255) NOT NULL DEFAULT ''
	);
	CREATE TABLE credentials (
	    id INT AUTO_INCREMENT PRIMARY KEY,
	    email VARCHAR(255) NOT NULL DEFAULT '',
	    password VARCHAR(255) NOT NULL DEFAULT ''
	);
	INSERT INTO domains VALUES (1, "example.com");
	INSERT INTO domains VALUES (2, "example.net");
	INSERT INTO domains VALUES (3, "example.org");
	
	INSERT INTO virtuals VALUES (1, "abuse@example.com", "bob@example.com");
	INSERT INTO virtuals VALUES (2, "postmaster@example.com", "bob@example.com");
	INSERT INTO virtuals VALUES (3, "webmaster@example.com", "bob@example.com");
	INSERT INTO virtuals VALUES (4, "bob@example.com", "vmail");
	INSERT INTO virtuals VALUES (5, "abuse@example.net", "alice@example.net");
	INSERT INTO virtuals VALUES (6, "postmaster@example.net", "alice@example.net");
	INSERT INTO virtuals VALUES (7, "webmaster@example.net", "alice@example.net");
	INSERT INTO virtuals VALUES (8, "alice@example.net", "vmail");
	
	INSERT INTO credentials VALUES (1, "bob@example.com", "$2b$08$ANGFKBL.BnDLL0bUl7I6aumTCLRJSQluSQLuueWRG.xceworWrUIu");
	INSERT INTO credentials VALUES (2, "alice@example.net", "$2b$08$AkHdB37kaj2NEoTcISHSYOCEBA5vyW1RcD8H1HG.XX0P/G1KIYwii");

*/etc/mail/mysql.conf*

	host db.example.com
	username maildba
	password OpenSMTPDRules!
	database opensmtpdb
	query_alias SELECT destination FROM virtuals WHERE email=?;
	query_credentials SELECT email, password FROM credentials WHERE email=?;
	query_domain SELECT domain FROM domains WHERE domain=?;

*/etc/mail/smtpd.conf*

	table domains mysql:/etc/mail/mysql.conf
	table virtuals mysql:/etc/mail/mysql.conf
	table credentials mysql:/etc/mail/mysql.conf
	listen on egress port 25 tls pki mail.example.com
	listen on egress port 587 tls-require pki mail.example.com auth <credentials>
	accept from any for domain <domains> virtual <virtuals> deliver to mbox

## MOVING FROM POSTFIX (& POSTFIXADMIN)

*/etc/mail/mysql.conf*

	host db.example.com
	username postfix
	password PostfixOutOpenSMTPDin
	database postfix
	query_alias SELECT destination FROM alias WHERE email=?;
	query_credentials SELECT username, password FROM mailbox WHERE username=?;
	query_domain SELECT domain FROM domain WHERE domain=?;

The rest of the config remains the same.

# FILES

*/etc/mail/mysql.conf*

> Default
> table-mysql(8)
> configuration file.

# TODO

Documenting the following query options:

	**query_netaddr**
	**query_userinfo**
	**query_source**
	**query_mailaddr**
	**query_addrname**

# SEE ALSO

smtpd.conf(5),
smtpctl(8),
smtpd(8),
encrypt(1),
crypt(3)

OpenBSD 7.5 - July 4, 2016
