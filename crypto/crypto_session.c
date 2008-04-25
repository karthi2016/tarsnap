#include "bsdtar_platform.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/aes.h>

#include "crypto_internal.h"
#include "sysendian.h"
#include "warnp.h"

#include "crypto.h"

static void aes_ctr_stream(AES_KEY *, uint64_t *, uint8_t[16],
    const uint8_t *, uint8_t *, size_t);

struct crypto_session_internal {
	AES_KEY encr_write;
	uint64_t encr_write_bytectr;
	uint8_t encr_write_buf[16];
	uint8_t auth_write[32];
	uint64_t auth_write_nonce;
	AES_KEY encr_read;
	uint64_t encr_read_bytectr;
	uint8_t encr_read_buf[16];
	uint8_t auth_read[32];
	uint64_t auth_read_nonce;
};

/**
 * aes_ctr_stream(key, bytectr, buf, inbuf, outbuf, buflen):
 * Perform buflen bytes of AES-CTR.
 */
static void
aes_ctr_stream(AES_KEY * key, uint64_t * bytectr, uint8_t buf[16],
    const uint8_t * inbuf, uint8_t * outbuf, size_t buflen)
{
	uint8_t pblk[16];
	size_t pos;
	int bytemod;

	for (pos = 0; pos < buflen; pos++) {
		/* How far through the buffer are we? */
		bytemod = *bytectr % 16;

		/* Generate a block of cipherstream if needed. */
		if (bytemod == 0) {
			be64enc(pblk, 0);
			be64enc(pblk + 8, *bytectr / 16);
			AES_encrypt(pblk, buf, key);
		}

		/* Encrypt a byte. */
		outbuf[pos] = inbuf[pos] ^ buf[bytemod];

		/* Move to the next byte of cipherstream. */
		*bytectr = *bytectr + 1;
	}
}

/**
 * crypto_session_init(pub, priv, nonce, mkey, encr_write, auth_write,
 *     encr_read, auth_read):
 * Compute K = ${pub}^(2^258 + ${priv}), mkey = MGF1(nonce || K, 48), and
 * return a CRYPTO_SESSION with encryption and authentication write and read
 * keys constructed from HMAC(mkey, (encr|auth)_(write|read)).
 */
CRYPTO_SESSION *
crypto_session_init(uint8_t pub[CRYPTO_DH_PUBLEN],
    uint8_t priv[CRYPTO_DH_PRIVLEN], uint8_t nonce[32], uint8_t mkey[48],
    const char * encr_write, const char * auth_write,
    const char * encr_read, const char * auth_read)
{
	struct crypto_session_internal * CS;
	uint8_t K[CRYPTO_DH_PUBLEN];
	uint8_t MGFbuf[32 + CRYPTO_DH_PUBLEN];
	uint8_t aes_write[32];
	uint8_t aes_read[32];

	/* Compute K = 2^(xy) mod p. */
	if (crypto_dh_compute(pub, priv, K))
		goto err0;

	/* Shared key is MGF1(nonce || K, 48). */
	memcpy(MGFbuf, nonce, 32);
	memcpy(MGFbuf + 32, K, CRYPTO_DH_PUBLEN);
	crypto_MGF1(MGFbuf, 32 + CRYPTO_DH_PUBLEN, mkey, 48);

	/* Allocate space for session key stucture. */
	if ((CS = malloc(sizeof(struct crypto_session_internal))) == NULL)
		goto err0;

	/* Generate raw keys. */
	crypto_hash_data_key(mkey, 48,
	    (const uint8_t *)encr_write, strlen(encr_write), aes_write);
	crypto_hash_data_key(mkey, 48,
	    (const uint8_t *)auth_write, strlen(auth_write), CS->auth_write);
	crypto_hash_data_key(mkey, 48,
	    (const uint8_t *)encr_read, strlen(encr_read), aes_read);
	crypto_hash_data_key(mkey, 48,
	    (const uint8_t *)auth_read, strlen(auth_read), CS->auth_read);

	/* Expand AES keys. */
	if (AES_set_encrypt_key(aes_write, 256, &CS->encr_write)) {
		warn0("error in AES_set_encrypt_key");
		goto err1;
	}
	if (AES_set_encrypt_key(aes_read, 256, &CS->encr_read)) {
		warn0("error in AES_set_encrypt_key");
		goto err1;
	}

	/* Initialize parameters. */
	CS->encr_write_bytectr = CS->encr_read_bytectr = 0;
	CS->auth_write_nonce = CS->auth_read_nonce = 0;

	/* Success! */
	return (CS);

err1:
	free(CS);
err0:
	/* Failure! */
	return (NULL);
}

/**
 * crypto_session_encrypt(CS, inbuf, outbuf, buflen):
 * Encrypt inbuf with the session write key and write ciphertext to outbuf.
 */
void
crypto_session_encrypt(CRYPTO_SESSION * CS, const uint8_t * inbuf,
    uint8_t * outbuf, size_t buflen)
{

	/* Call AES-CTR helper function. */
	aes_ctr_stream(&CS->encr_write, &CS->encr_write_bytectr,
	    CS->encr_write_buf, inbuf, outbuf, buflen);
}

/**
 * crypto_session_decrypt(CS, inbuf, outbuf, buflen):
 * Decrypt inbuf with the session read key and write plaintext to outbuf.
 */
void
crypto_session_decrypt(CRYPTO_SESSION * CS, const uint8_t * inbuf,
    uint8_t * outbuf, size_t buflen)
{

	/* Call AES-CTR helper function. */
	aes_ctr_stream(&CS->encr_read, &CS->encr_read_bytectr,
	    CS->encr_read_buf, inbuf, outbuf, buflen);
}

/**
 * crypto_session_sign(CS, buf, buflen, sig):
 * Generate sig = write_auth(buf).
 */
void
crypto_session_sign(CRYPTO_SESSION * CS, const uint8_t * buf, size_t buflen,
    uint8_t sig[32])
{
	uint8_t nonce[8];

	/* Convert nonce to 8-byte big-endian format, and increment. */
	be64enc(nonce, CS->auth_write_nonce);
	CS->auth_write_nonce += 1;

	/* Generate hash. */
	crypto_hash_data_key_2(CS->auth_write, 32,
	    nonce, 8, buf, buflen, sig);
}

/**
 * crypto_session_verify(CS, buf, buflen, sig):
 * Verify that sig = read_auth(buf).  Return non-zero if the signature
 * does not match.
 */
int
crypto_session_verify(CRYPTO_SESSION * CS, const uint8_t * buf, size_t buflen,
    const uint8_t sig[32])
{
	uint8_t nonce[8];
	uint8_t sig_actual[32];

	/* Convert nonce to 8-byte big-endian format, and increment. */
	be64enc(nonce, CS->auth_read_nonce);
	CS->auth_read_nonce += 1;

	/* Generate hash. */
	crypto_hash_data_key_2(CS->auth_read, 32,
	    nonce, 8, buf, buflen, sig_actual);

	/* Determine if the signatures match. */
	if (crypto_verify_bytes(sig, sig_actual, 32))
		return (1);
	else
		return (0);
}

/**
 * crypto_session_free(CS):
 * Free a CRYPTO_SESSION structure.
 */
void
crypto_session_free(CRYPTO_SESSION * CS)
{

	free(CS);
}
