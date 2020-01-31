# PureSpice

A pure C implementation of the spice protocol as used by the Looking Glass
project. This implementation unlike libspice does not require or rely on glib in
any way.

## Note

This project's goal has been to provide keyboard and mouse input for the Looking
Glass project where the host and client run on the same physical system and as
such is missing encryption support.
