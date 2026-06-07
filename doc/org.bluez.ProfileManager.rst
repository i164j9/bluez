========================
org.bluez.ProfileManager
========================

--------------------------------------------
BlueZ D-Bus ProfileManager API documentation
--------------------------------------------

:Version: BlueZ
:Date: October 2023
:Manual section: 5
:Manual group: Linux System Administration

Interface
=========

:Service:	org.bluez
:Interface:	org.bluez.ProfileManager1
:Object path:	/org/bluez

Methods
-------

void RegisterProfile(object profile, string uuid, dict options)
```````````````````````````````````````````````````````````````

Registers profile agent.

The object path defines the path of the profile that will be called when there
is a connection and must implement **org.bluez.Profile(5)** interface.

If an application disconnects from the bus all its registered profiles will be
removed.

Possible uuid values:

:"0000111f-0000-1000-8000-00805f9b34fb":

	HFP AG, default profile Version is 1.7, profile Features is 0b001001 and
	RFCOMM channel is 13. Authentication is required.

:"0000111e-0000-1000-8000-00805f9b34fb":

	HFP HS, default profile Version is 1.7, profile Features is 0b000000 and
	RFCOMM channel is 7. Authentication is required.

:"00001112-0000-1000-8000-00805f9b34fb":

	HSP AG, default profile Version is 1.2, RFCOMM channel is 12 and
	Authentication is required. Does not support any Features, option is
	ignored.

:"00001108-0000-1000-8000-00805f9b34fb":

	HSP HS, default profile Version is 1.2, profile Features is 0b0 and
	RFCOMM channel is 6. Authentication is required.

	Features is one bit value, specify capability of Remote Audio Volume
	Control (by default turned off).

:"00001101-0000-1000-8000-00805f9b34fb":

	SPP, default profile Version is 1.2. For server registrations BlueZ
	allocates an RFCOMM channel dynamically when Channel is omitted or set
	to 0, and publishes the generated SDP record.

:"<vendor UUID>":

	Vendor defined UUID, no defaults, must set options.

Possible options values:

:string Name:

	Human readable name for the profile

	For SPP client registrations, when a remote device exposes multiple
	Serial Port SDP records for the same UUID, BlueZ uses Name to prefer a
	unique remote service-name match during ConnectProfile. If no unique
	match is available, the connect attempt is rejected instead of silently
	choosing the first record.

:string Service:

	The primary service class UUID (if different from the actual profile
	UUID).

:string Role:

	For asymmetric profiles that do not have UUIDs available to uniquely
	identify each side this parameter allows specifying the precise local
	role.

	Possible values:

	:"client":
	:"server":

:uint16 Channel:

	RFCOMM channel number that is used for client and server UUIDs.

	If applicable it will be used in the SDP record as well.

	Setting Channel to 0 requests daemon-assigned dynamic RFCOMM channel
	allocation. For SPP this is the recommended server configuration.

:uint16 PSM:

	PSM number that is used for client and server UUIDs.

	If applicable it will be used in the SDP record as well.

:boolean RequireAuthentication:

	Pairing is required before connections will be established.

	No devices will be connected if not paired.

:boolean RequireAuthorization:

	Request authorization before any connection will be established.

	For RFCOMM server transports BlueZ uses deferred setup so
	authorization is resolved before the profile connection is completed.

:boolean AutoConnect:

	In case of a client UUID this will force connection of the RFCOMM or
	L2CAP channels when a remote device is connected.

:string ServiceRecord:

	Provide a manual SDP record.

	For SPP this is only accepted for server roles with an explicit,
	non-zero Channel. The record must advertise the Serial Port service
	class, Public Browse Group, the expected Serial Port profile version,
	and the same RFCOMM channel that BlueZ listens on.

	The daemon-generated record is the recommended path for SPP
	interoperability and qualification.

:uint16 Version:

	Profile version (for SDP record)

:uint16 Features:

	Profile features (for SDP record)

Possible errors:

:org.bluez.Error.InvalidArguments:
:org.bluez.Error.AlreadyExists:

void UnregisterProfile(object profile)
``````````````````````````````````````

Unregisters profile object that has been previously registered using
**RegisterProfile**.

The object path parameter must match the same value that has been used on
registration.

Possible errors:

:org.bluez.Error.DoesNotExist:
