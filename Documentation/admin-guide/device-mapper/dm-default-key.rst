.. SPDX-License-Identifier: GPL-2.0

==============
dm-default-key
==============

The ``default-key`` target implements Android metadata encryption. It encrypts
filesystem metadata and unencrypted file contents, while passing through fscrypt
file contents without applying a second layer of encryption. It supports
AES-256-XTS and Adiantum through blk-crypto.

This target is adapted from Android common and the Lenovo Android kernel.
It requires ``CONFIG_DM_DEFAULT_KEY`` and ``CONFIG_FS_ENCRYPTION_INLINE_CRYPT``.
Filesystems must mark encrypted file I/O with the fscrypt bio helpers, including
when copying ciphertext without a key. The integration covers ext4 and F2FS.
The metadata key must be protected at least as strongly as the file keys.

Table format::

    <cipher> <hex_key> <iv_offset> <device> <start> [<count> <options>]

The cipher is ``aes-xts-plain64`` or ``xchacha12,aes-adiantum-plain64``.
Offsets and target lengths are in 512-byte sectors. Optional arguments are
``allow_discards``, ``sector_size:512|1024|2048|4096``, ``iv_large_sectors``, and
``wrappedkey_v0``. Non-default sector sizes require ``iv_large_sectors``;
the IV offset and target length must be aligned to the encryption sector size.
The sector size and IV layout must match the existing volume.

``wrappedkey_v0`` selects a hardware-wrapped metadata key. Supply a valid
ephemerally wrapped key for the current boot, not the encrypted key-storage
files from Android's metadata partition. Software fallback handles raw keys
only. The target's table status omits key material.

Android file keys are separate from the metadata key. The legacy
``__FSCRYPT_ADD_KEY_FLAG_HW_WRAPPED`` flag in ``fscrypt_add_key_arg.__flags``
selects Android's original wrapped-key identifier derivation. The upstream
``FSCRYPT_ADD_KEY_FLAG_HW_WRAPPED`` flag in ``flags`` retains its distinct
identifier derivation. Specifying both is invalid. Neither interface obtains
keys from Android KeyMint, authenticates the user's credential, or unlocks
files by itself.

Read-only access to an existing Android filesystem
=================================================

First obtain the correct metadata key through compatible userspace and the
device's TEE. Create a read-only mapping with ``dmsetup create --readonly``,
passing the table on standard input to avoid putting key material in command
arguments. Keep the underlying partition read-only as well. Do not enable
discards. For F2FS, mount the mapping with ``ro,norecovery,inlinecrypt`` to
prevent roll-forward recovery; for ext4, use ``ro,noload,inlinecrypt``.

Loading the metadata key only exposes the filesystem structure. Reading
encrypted directories and files additionally requires installing their DE/CE
keys with ``FS_IOC_ADD_ENCRYPTION_KEY`` on the mounted filesystem. Use the
legacy flag for an Android ``wrappedkey_v0`` filesystem.

Qualcomm ICE requires ``qcom_ice.use_wrapped_keys=1`` at boot to opt into HWKM.
Its default is raw-key mode. Availability also depends on firmware support for
the required SCM calls. Enabling this parameter does not recover Android keys.

References:

* https://source.android.com/docs/security/features/encryption/metadata
* https://android.googlesource.com/kernel/common/+/refs/heads/android-mainline/drivers/md/dm-default-key.c
* https://android.googlesource.com/kernel/common/+/refs/heads/android-mainline/fs/crypto/keyring.c
