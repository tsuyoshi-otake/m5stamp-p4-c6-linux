/*
 * EasyStick M3 Dropbear policy.
 *
 * Password authentication is disabled at compile time.  The serial/CDC
 * first-boot flow may set a local console password for the p4 account, but
 * that password is never accepted by the SSH server.  Public-key support is
 * deliberately retained for standard OpenSSH clients.
 */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
