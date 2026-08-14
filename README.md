# Wired Server 2.5

Wired Server is BBS-oriented server for UNIX-based operating systems. It uses [libwired](https://github.com/nark/libwired) and provide an implementation of the Wired 2.0 protocol. This project is a fork of the original Wired Server developed by Axel Andersson at [Zanka Software](http://zankasoftware.com/).

### Requirements

This program is mainly tested on Debian/Ubuntu Linux distributions, FreeBSD and Mac OS X. The source code is under BSD license and is totally free for use with respect of its attributed license.

To install the Wired server, you need the following prerequisites:

* **OpenSSL**: [http://www.openssl.org/source/](http://www.openssl.org/source/)
* **libxml2**: [http://xmlsoft.org/](http://xmlsoft.org/)
* **zlib**: [http://zlib.net/](http://zlib.net/)
* **git**: [http://git-scm.com/](http://git-scm.com/)

These are usually distributed with operating systems.

#### Howto install on:

**Debian/Ubuntu**

	sudo apt-get install -y build-essential autoconf screen git libsqlite3-dev libxml2-dev libssl-dev zlib1g-dev autotools-dev automake

**Archlinux**

	sudo pacman -Sy base-devel autoconf screen git sqlite3 libxml2 zlib

**CentOS 7**

	sudo yum -y install screen git libtool openssl-devel sqlite-devel.x86_64 libxml2-devel zlib-devel autoconf gcc make

**CentOS 8 / Fedora 28/29/30/31 (and probably even older versions of Fedora)**

	sudo yum -y install screen git libtool openssl-devel sqlite-devel libxml2-devel zlib-devel autoconf gcc make

**openSUSE Leap 42.3 (and probably other Versions)**

	sudo zypper install -t pattern devel_basis 
	sudo zypper install screen git sqlite3-devel libxml2-devel libz1 openssl-devel

### Getting started

Installing Wired Server from sources will be done using the Autotools standard (configure, make, make install).

##### 1. Get Wired Server sources via Terminal (git must be installed!):

	git clone https://github.com/ProfDrLuigi/wired wired

Then move to the `wired` directory:

	cd wired/

Initialize and update submodules repositories:

	git clone https://github.com/ProfDrLuigi/libwired libwired
	libwired/bootstrap

Then check that the `libwired` directory was not empty and `configure` file exists.

##### 3. Run the configuration script:

During the configuration, scripts will check that your environment fills the requirements described at the top of this document. You will be warned if any of the required component is missing on your operating system.

To start configuration for Intel/AMD, use the following command:

	./configure

To start configuration for arm64, use the following command:

	./configure --build=aarch64 --host=aarch64

Wired Server is designed to be installed into `/usr/local` by default. To change this, run:

	./configure --prefix=/path	

To change the default user the installation will write files as, run:

	./configure --with-user=USER

If you installed OpenSSL in a non-standard path, use the following command example as reference:

	env CPPFLAGS=-I/usr/local/opt/openssl/include \
	     LDFLAGS=-L/usr/local/opt/openssl/lib ./configure

Use `./configure --help` in order to display more options.



##### 4. Compile source code:

On GNU-like unices, type:

	make

Or, on BSD-like unices, type: 

	gmake

##### 5. Install on your system:

On GNU-like unices, type:

	make install

Or, on BSD-like unices, type: 

	gmake install


This will require write permissions to `/usr/local/`, or whatever directory you set as the prefix above. Depending of your OS setup, you may require to use `sudo`.

### WiredBot: file announcements and local chat messages

Wired Server includes a virtual chat user named **WiredBot**. It can announce
new files and folders from a watched directory and can accept chat messages
from local programs through a named pipe. Both inputs use the same configurable
name, status and icon and post to the public chat like a regular user.

#### Installed files

`make install` prepares these resources in the Wired installation directory:

* `robo.png` — the default WiredBot icon.
* `WiredBot` — a named pipe (FIFO), owned by the configured Wired user and
  group with mode `0660`.

Existing copies are preserved during updates. Installation stops with an error
instead of overwriting `WiredBot` if that path exists but is not a named pipe.

The shipped configuration refers to both resources but keeps the bot disabled
until it is explicitly enabled:

```ini
watch enabled = no
watch pipe = WiredBot
watch icon = robo.png
```

#### Complete configuration example

Add or edit the following settings in `<wired-root>/etc/wired.conf`:

```ini
# Enable the virtual user, directory watcher and local message pipe.
watch enabled = yes

# Optional directory monitored recursively for new files and folders.
watch path = files/Uploads

# Message used for file and folder announcements.
watch message = New stuff has arrived: $FILE ($SIZE)

# Local FIFO used by scripts and other local processes.
watch pipe = WiredBot

# Identity shown in the public chat user list.
watch name = WiredBot
watch status = Watching for new files
watch icon = robo.png
```

Relative paths are resolved from the Wired installation directory. Absolute
paths may also be used. After changing the configuration, validate and reload
it without restarting the server:

```sh
<wired-root>/wiredctl configtest
<wired-root>/wiredctl reload
```

For example, with Wired installed in `/opt/wired`:

```sh
/opt/wired/wiredctl configtest
/opt/wired/wiredctl reload
```

#### Directory watcher behavior

When `watch path` is configured, the server scans that directory recursively:

* Items already present when the server starts or reloads its configuration
  form the initial baseline and are not announced.
* A new file is announced only after its size and modification time have
  stopped changing.
* A newly copied directory is announced once as a directory. Files and nested
  directories already arriving inside it are added to the baseline silently
  and are not announced individually.
* Files created later inside an already known directory are announced normally.
* Invisible paths are ignored.

The announcement template supports these placeholders:

* `$FILE` — path relative to `watch path`.
* `$SIZE` — formatted file size; for a directory this becomes `folder`.

Examples:

```ini
watch message = New stuff has arrived: $FILE
```

```ini
watch message = New item: $FILE ($SIZE)
```

#### Sending messages from Debian programs

Every newline-terminated line written to the FIFO becomes one public-chat
message from WiredBot. With Wired installed in `/opt/wired`, use:

```sh
echo "Hello World." > /opt/wired/WiredBot
```

Multiple lines produce multiple chat messages:

```sh
printf 'First message\nSecond message\n' > /opt/wired/WiredBot
```

Programs running as another Unix user require write permission through the
FIFO's owner or group. Prefer adding that user to the configured Wired group
instead of making the pipe world-writable.

Messages may contain HTML supported by Wired Client. Two convenience aliases
are also accepted:

* `<n>...</n>` creates a neutral message container.
* `<bold>...</bold>` renders its contents in bold.

For example:

```sh
echo '<n>☹️ <bold>Hello</bold></n>' > /opt/wired/WiredBot
```

This is delivered as:

```html
<span>☹️ <strong>Hello</strong></span>
```

HTML display must be enabled in the receiving Wired Client. Because FIFO input
may contain HTML, grant write access only to trusted local users and services.
Messages larger than 64 KiB are discarded.

#### Changing the bot identity

The virtual user's appearance is controlled entirely through `wired.conf`:

```ini
watch name = Wired Server
watch status = File notifications
watch icon = robo.png
```

The icon may use a relative path below the Wired installation directory or an
absolute path. Identity changes take effect after `wiredctl reload` and are
broadcast to connected clients.

#### Disabling WiredBot

To remove the virtual user from the public chat and stop both the directory
watcher and FIFO processing, set:

```ini
watch enabled = no
```

Then reload the configuration. The FIFO may remain in the installation
directory so it is immediately available the next time the bot is enabled.

##### 6. Running Wired Server

By default a user with the login "admin" and no password is created. Use Wired Client or Wire to connect to your newly installed Wired Server. 

Wired is compiled as foreground Task in Debug mode. Thats why you nee to run it in a screen-Session.
If you run it in Daemon Mode your CPU will going crazy after some time (100% usage).

To start an installed Wired server, run:

	./wiredctl start

### Get More

If you are interested in the Wired project, check the Website at [https://wired.read-write.fr/](https://wired.read-write.fr)

### Troubleshootings

This implementation of the Wired 2.0/2.5 protocol is not compliant with the version of the protocol distributed by Zanka Software, for several deep technical reasons.
