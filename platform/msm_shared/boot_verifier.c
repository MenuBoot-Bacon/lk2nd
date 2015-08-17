/*
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 *  * Neither the name of The Linux Foundation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.

 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdint.h>
#include <crypto_hash.h>
#include <boot_verifier.h>
#include <image_verify.h>
#include <mmc.h>
#include <oem_keystore.h>
#include <openssl/asn1t.h>
#include <openssl/x509.h>
#include <partition_parser.h>
#include <rsa.h>
#include <string.h>
#include <openssl/err.h>
#include <qseecom_lk_api.h>
#include <secapp_loader.h>
#include <target.h>

static KEYSTORE *oem_keystore;
static KEYSTORE *user_keystore;
static uint32_t dev_boot_state = RED;
char KEYSTORE_PTN_NAME[] = "keystore";
RSA *rsa_from_cert = NULL;

ASN1_SEQUENCE(AUTH_ATTR) ={
	ASN1_SIMPLE(AUTH_ATTR, target, ASN1_PRINTABLESTRING),
	ASN1_SIMPLE(AUTH_ATTR, len, ASN1_INTEGER)
} ASN1_SEQUENCE_END(AUTH_ATTR)
IMPLEMENT_ASN1_FUNCTIONS(AUTH_ATTR)

	ASN1_SEQUENCE(VERIFIED_BOOT_SIG) = {
		ASN1_SIMPLE(VERIFIED_BOOT_SIG, version, ASN1_INTEGER),
		ASN1_SIMPLE(VERIFIED_BOOT_SIG, certificate, X509),
		ASN1_SIMPLE(VERIFIED_BOOT_SIG, algor, X509_ALGOR),
		ASN1_SIMPLE(VERIFIED_BOOT_SIG, auth_attr, AUTH_ATTR),
		ASN1_SIMPLE(VERIFIED_BOOT_SIG, sig, ASN1_OCTET_STRING)
	} ASN1_SEQUENCE_END(VERIFIED_BOOT_SIG)
IMPLEMENT_ASN1_FUNCTIONS(VERIFIED_BOOT_SIG)

	ASN1_SEQUENCE(KEY) = {
		ASN1_SIMPLE(KEY, algorithm_id, X509_ALGOR),
		ASN1_SIMPLE(KEY, key_material, RSAPublicKey)
	}ASN1_SEQUENCE_END(KEY)
IMPLEMENT_ASN1_FUNCTIONS(KEY);

ASN1_SEQUENCE(KEYBAG) = {
	ASN1_SIMPLE(KEYBAG, mykey, KEY)
}ASN1_SEQUENCE_END(KEYBAG)
IMPLEMENT_ASN1_FUNCTIONS(KEYBAG)

	ASN1_SEQUENCE(KEYSTORE_INNER) = {
		ASN1_SIMPLE(KEYSTORE_INNER, version, ASN1_INTEGER),
		ASN1_SIMPLE(KEYSTORE_INNER, mykeybag, KEYBAG)
	} ASN1_SEQUENCE_END(KEYSTORE_INNER)
IMPLEMENT_ASN1_FUNCTIONS(KEYSTORE_INNER)

	ASN1_SEQUENCE(KEYSTORE) = {
		ASN1_SIMPLE(KEYSTORE, version, ASN1_INTEGER),
		ASN1_SIMPLE(KEYSTORE, mykeybag, KEYBAG),
		ASN1_SIMPLE(KEYSTORE, sig, VERIFIED_BOOT_SIG)
	} ASN1_SEQUENCE_END(KEYSTORE)
IMPLEMENT_ASN1_FUNCTIONS(KEYSTORE)

static uint32_t read_der_message_length(unsigned char* input)
{
	uint32_t len = 0;
	int pos = 0;
	uint8_t len_bytes = 1;

	/* Check if input starts with Sequence id (0X30) */
	if(input[pos] != 0x30)
		return len;
	pos++;

	/* A length of 0xAABBCCDD in DER encoded messages would be sequence of
	   following octets 0xAA, 0xBB, 0XCC, 0XDD.

	   To read length - read each octet and shift left by 1 octect before
	   reading next octet.
	*/
	/* check if short or long length form */
	if(input[pos] & 0x80)
	{
		len_bytes = (input[pos] & ~(0x80));
		pos++;
	}
	while(len_bytes)
	{
		/* Shift len by 1 octet */
		len = len << 8;

		/* Read next octet */
		len = len | input[pos];
		pos++; len_bytes--;
	}

	/* Add number of octets representing sequence id and length  */
	len += pos;

	return len;
}

static int verify_digest(unsigned char* input, unsigned char *digest, int hash_size)
{
	int ret = -1;
	X509_SIG *sig = NULL;
	uint32_t len = read_der_message_length(input);
	if(!len)
	{
		dprintf(CRITICAL, "boot_verifier: Signature length is invalid.\n");
		return ret;
	}

	sig = d2i_X509_SIG(NULL, (const unsigned char **) &input, len);
	if(sig == NULL)
	{
		dprintf(CRITICAL, "boot_verifier: Reading digest failed\n");
		return ret;
	}

	if(sig->digest->length != SHA256_SIZE)
	{
		dprintf(CRITICAL, "boot_verifier: Digest length error.\n");
		goto verify_digest_error;
	}

	if(memcmp(sig->digest->data, digest, hash_size) == 0)
		ret = 0;

verify_digest_error:
	if(sig != NULL)
		X509_SIG_free(sig);

	return ret;
}

static int add_attribute_to_img(unsigned char *ptr, AUTH_ATTR *input)
{
	return i2d_AUTH_ATTR(input, &ptr);
}

static bool boot_verify_compare_sha256(unsigned char *image_ptr,
		unsigned int image_size, unsigned char *signature_ptr, RSA *rsa)
{
	int ret = -1;
	bool auth = false;
	unsigned char *plain_text = NULL;
	unsigned int digest[8];

	plain_text = (unsigned char *)calloc(sizeof(char), SIGNATURE_SIZE);
	if (plain_text == NULL) {
		dprintf(CRITICAL, "boot_verifier: Calloc failed during verification\n");
		goto cleanup;
	}

	/* Calculate SHA256sum */
	image_find_digest(image_ptr, image_size, CRYPTO_AUTH_ALG_SHA256,
			(unsigned char *)&digest);

	/* Find digest from the image */
	ret = image_decrypt_signature_rsa(signature_ptr, plain_text, rsa);

	dprintf(SPEW, "boot_verifier: Return of RSA_public_decrypt = %d\n",
			ret);

	ret = verify_digest(plain_text, (unsigned char*)digest, SHA256_SIZE);
	if(ret == 0)
	{
		auth = true;
#ifdef TZ_SAVE_KERNEL_HASH
		save_kernel_hash((unsigned char *) &digest, CRYPTO_AUTH_ALG_SHA256);
#endif
	}

cleanup:
	if (plain_text != NULL)
		free(plain_text);
	EVP_cleanup();
	CRYPTO_cleanup_all_ex_data();
	ERR_remove_thread_state(NULL);
	return auth;

}

static bool verify_image_with_sig(unsigned char* img_addr, uint32_t img_size,
		char *pname, VERIFIED_BOOT_SIG *sig, KEYSTORE *ks)
{
	bool ret = false;
	uint32_t len;
	int shift_bytes;
	RSA *rsa = NULL;
	bool keystore_verification = false;
	EVP_PKEY* key = NULL;

	if(!strcmp(pname, "keystore"))
		keystore_verification = true;

	/* Verify target name */
	if(strncmp((char*)(sig->auth_attr->target->data), pname,
				sig->auth_attr->target->length) ||
				(strlen(pname) != (unsigned long) sig->auth_attr->target->length))
	{
		dprintf(CRITICAL,
				"boot_verifier: verification failure due to target name mismatch\n");
		goto verify_image_with_sig_error;
	}
	/* Read image size from signature */
	/* A len = 0xAABBCC (represented by 3 octets) would be stored in
	   len->data as 0X00CCBBAA and len->length as 3(octets).

	   To read len we need to left shift data to number of missing octets and
	   then change it to host long
	 */
	len = *((uint32_t*)sig->auth_attr->len->data);
	shift_bytes = sizeof(uint32_t) - sig->auth_attr->len->length;
	if(shift_bytes > 0) {
		len = len << (shift_bytes*8);
	}
	len = ntohl(len);

	/* Verify image size*/
	if(len != img_size)
	{
		dprintf(CRITICAL,
				"boot_verifier: image length is different. (%d vs %d)\n",
				len, img_size);
		goto verify_image_with_sig_error;
	}

	/* append attribute to image */
	if(!keystore_verification)
	{
		// verifying a non keystore partition
		img_size += add_attribute_to_img((unsigned char*)(img_addr + img_size),
				sig->auth_attr);
	}

	/* compare SHA256SUM of image with value in signature */
	if(ks != NULL)
	{
		// use rsa from keystore
		rsa = ks->mykeybag->mykey->key_material;
	}
	else
	{
		dprintf(CRITICAL, "%s:%d: Keystore is null\n", __func__, __LINE__);
		ASSERT(0);
	}

	// verify boot.img with rsa from oem keystore
	if((ret = boot_verify_compare_sha256(img_addr, img_size,
			(unsigned char*)sig->sig->data, rsa)))

	{
		dprintf(SPEW, "Verified boot.img with oem keystore\n");
		boot_verify_send_event(BOOTIMG_KEYSTORE_VERIFICATION_PASS);
		goto verify_image_with_sig_done;
	}
	else
	{
		dprintf(INFO, "Verification with oem keystore failed. Use embedded certificate for verification\n");
		// get the public key from certificate in boot.img
		if ((key = X509_get_pubkey(sig->certificate)))
		{
			// convert to rsa key format
			dprintf(INFO, "RSA KEY found from the embedded certificate\n");
			rsa = EVP_PKEY_get1_RSA(key);
			rsa_from_cert = rsa;
		}
		else
		{
			dprintf(CRITICAL, "Unable to extract public key from certificate\n");
			ASSERT(0);
		}
	}

	// verify boot.img with rsa from embedded certificate
	if ((ret = boot_verify_compare_sha256(img_addr, img_size,
			(unsigned char*)sig->sig->data, rsa)))
	{
		dprintf(SPEW, "Verified boot.img with embedded certificate in boot image\n");
		boot_verify_send_event(BOOTIMG_EMBEDDED_CERT_VERIFICATION_PASS);
		goto verify_image_with_sig_done;
	}
	else
	{
		dprintf(INFO, "verified for red state\n");
		boot_verify_send_event(BOOTIMG_VERIFICATION_FAIL);
		goto verify_image_with_sig_done;
	}

verify_image_with_sig_error:
verify_image_with_sig_done:
	return ret;
}

static int encode_inner_keystore(unsigned char *ptr, KEYSTORE *ks)
{
	int ret = 0;
	KEYSTORE_INNER  *ks_inner = KEYSTORE_INNER_new();
	if (ks_inner == NULL)
		return ret;
	ASN1_INTEGER *tmp_version = ks_inner->version;
	KEYBAG *tmp_mykeybag = ks_inner->mykeybag;

	ks_inner->version = ks->version;
	ks_inner->mykeybag = ks->mykeybag;
	ret = i2d_KEYSTORE_INNER(ks_inner, &ptr);

	ks_inner->version = tmp_version;
	ks_inner->mykeybag = tmp_mykeybag;

	if(ks_inner != NULL)
		KEYSTORE_INNER_free(ks_inner);
	return ret;
}

static bool verify_keystore(unsigned char * ks_addr, KEYSTORE *ks)
{
	bool ret = false;
	unsigned char * ptr = ks_addr;
	uint32_t inner_len = encode_inner_keystore(ptr, ks);
	ret = verify_image_with_sig(ks_addr, inner_len, "keystore", ks->sig,
			oem_keystore);
	return ret;
}

static void read_oem_keystore()
{
	KEYSTORE *ks = NULL;
	uint32_t len = 0;
	const unsigned char *input = OEM_KEYSTORE;

	if(oem_keystore != NULL)
		return;

	len = read_der_message_length((unsigned char *)input);
	if(!len)
	{
		dprintf(CRITICAL, "boot_verifier: oem keystore length is invalid.\n");
		return;
	}

	ks = d2i_KEYSTORE(NULL, (const unsigned char **) &input, len);
	if(ks != NULL)
	{
		oem_keystore = ks;
		user_keystore = ks;
	}
}

uint32_t boot_verify_keystore_init()
{
	/* Read OEM Keystore */
	read_oem_keystore();

	return dev_boot_state;
}

bool send_rot_command(uint32_t is_unlocked)
{
	int ret = 0;
	unsigned char *input = NULL;
	char *rot_input = NULL;
	unsigned int digest[9] = {0}, final_digest[8] = {0};
	uint32_t auth_algo = CRYPTO_AUTH_ALG_SHA256;
	uint32_t boot_device_state = boot_verify_get_state();
	int app_handle = 0;
	uint32_t len_oem_rsa = 0, len_from_cert = 0;
	km_set_rot_req_t *read_req;
	km_set_rot_rsp_t read_rsp;
	app_handle = get_secapp_handle();
	int n = 0, e = 0;
	switch (boot_device_state)
	{
		case GREEN:
			// Locked device and boot.img verified against OEM keystore.
			// n is length of modulus and e is length exponent
			n = BN_num_bytes(oem_keystore->mykeybag->mykey->key_material->n);
			e = BN_num_bytes(oem_keystore->mykeybag->mykey->key_material->e);
			len_oem_rsa = n + e;
			if(!(input = malloc(len_oem_rsa)))
			{
				dprintf(CRITICAL, "Failed to allocate memory for ROT structure\n");
				ASSERT(0);
			}
			// convert the absolute value on n, e to big endian form
			BN_bn2bin(oem_keystore->mykeybag->mykey->key_material->n, input);
			BN_bn2bin(oem_keystore->mykeybag->mykey->key_material->e, input+n);
			// Hash of key from OEM KEYSTORE
			hash_find((unsigned char *)input, len_oem_rsa, (unsigned char *) &digest, auth_algo);
			digest[8] = is_unlocked;
			break;
		case YELLOW:
		case RED:
			// Locked device and boot.img passed (yellow) or failed (red) verification with the certificate embedded to the boot.img.
			if (!rsa_from_cert)
			{
				dprintf(CRITICAL, "RSA is null from the embedded certificate\n");
				ASSERT(0);
			}
			// n is length of modulus and e is length exponent
			n = BN_num_bytes(rsa_from_cert->n);
			e = BN_num_bytes(rsa_from_cert->e);
			len_from_cert = n + e;
			if(!(input = malloc(len_from_cert)))
			{
				dprintf(CRITICAL, "Failed to allocate memory for ROT structure\n");
				ASSERT(0);
			}
			// convert the absolute value on n, e to big endian form
			BN_bn2bin(rsa_from_cert->n, input);
			BN_bn2bin(rsa_from_cert->e, input+n);
			// Hash of key from certificate in boot image
			hash_find((unsigned char *)input, len_from_cert, (unsigned char *) &digest, auth_algo);
			digest[8] = is_unlocked;
			break;
		case ORANGE:
			// Unlocked device and no verification done.
			// Send the hash of boot device state
			input = NULL;
			digest[0] = is_unlocked;
			break;
	}
	// Hash of hash(key) + device state (locked/unlocked)
	hash_find((unsigned char *) digest, sizeof(digest), (unsigned char *)&final_digest, auth_algo);
	dprintf(SPEW, "Digest: ");
	for(uint8_t i = 0; i < 8; i++)
		dprintf(SPEW, "0x%x ", final_digest[i]);
	dprintf(SPEW, "\n");
	if(!(read_req = malloc(sizeof(km_set_rot_req_t) + sizeof(final_digest))))
	{
		dprintf(CRITICAL, "Failed to allocate memory for ROT structure\n");
		ASSERT(0);
	}

	void *cpy_ptr = (uint8_t *) read_req + sizeof(km_set_rot_req_t);
	// set ROT stucture
	read_req->cmd_id = KEYMASTER_SET_ROT;
	read_req->rot_ofset = (uint32_t) sizeof(km_set_rot_req_t);
	read_req->rot_size  = sizeof(final_digest);
	// copy the digest
	memcpy(cpy_ptr, (void *) &final_digest, sizeof(final_digest));
	dprintf(SPEW, "Sending Root of Trust to trustzone: start\n");

	ret = qseecom_send_command(app_handle, (void*) read_req, sizeof(km_set_rot_req_t) + sizeof(final_digest), (void*) &read_rsp, sizeof(read_rsp));
	if (ret < 0 || read_rsp.status < 0)
	{
		dprintf(CRITICAL, "QSEEcom command for Sending Root of Trust returned error: %d\n", read_rsp.status);
		if(input)
			free(input);
		free(read_req);
		free(rot_input);
		return false;
	}
	dprintf(SPEW, "Sending Root of Trust to trustzone: end\n");
	if(input)
		free(input);
	free(read_req);
	free(rot_input);
	return true;
}

bool boot_verify_image(unsigned char* img_addr, uint32_t img_size, char *pname)
{
	bool ret = false;
	VERIFIED_BOOT_SIG *sig = NULL;
	unsigned char* sig_addr = (unsigned char*)(img_addr + img_size);
	uint32_t sig_len = read_der_message_length(sig_addr);

	if(dev_boot_state == ORANGE)
	{
		dprintf(INFO, "boot_verifier: Device is in ORANGE boot state.\n");
		dprintf(INFO, "boot_verifier: Skipping boot verification.\n");
		return false;
	}

	if(!sig_len)
	{
		dprintf(CRITICAL, "boot_verifier: Error while reading signature length.\n");
		ASSERT(0);
	}

	if((sig = d2i_VERIFIED_BOOT_SIG(NULL, (const unsigned char **) &sig_addr, sig_len)) == NULL)
	{
		dprintf(CRITICAL,
				"boot_verifier: verification failure due to target name mismatch\n");
		ASSERT(0);
	}

	ret = verify_image_with_sig(img_addr, img_size, pname, sig, user_keystore);

	if(sig != NULL)
		VERIFIED_BOOT_SIG_free(sig);
	return ret;
}

void boot_verify_send_event(uint32_t event)
{
	switch(event)
	{
		case BOOT_INIT:
			dev_boot_state = GREEN;
			break;
		case BOOTIMG_KEYSTORE_VERIFICATION_PASS:
			dev_boot_state = GREEN;
			break;
		case BOOTIMG_EMBEDDED_CERT_VERIFICATION_PASS:
			if(dev_boot_state == GREEN)
				dev_boot_state = YELLOW;
			break;
		case BOOTIMG_VERIFICATION_FAIL:
			if(dev_boot_state == GREEN || dev_boot_state == YELLOW)
				dev_boot_state = RED;
			break;
		case DEV_UNLOCK:
			dev_boot_state = ORANGE;
			break;
		case USER_DENIES:
			if(dev_boot_state == YELLOW || dev_boot_state == ORANGE)
				dev_boot_state = RED;
			break;
	}
}

uint32_t boot_verify_get_state()
{
	return dev_boot_state;
}

void boot_verify_print_state()
{
	switch(dev_boot_state)
	{
		case GREEN:
			dprintf(INFO, "boot_verifier: Device is in GREEN boot state.\n");
			break;
		case ORANGE:
			dprintf(INFO, "boot_verifier: Device is in ORANGE boot state.\n");
			break;
		case YELLOW:
			dprintf(INFO, "boot_verifier: Device is in YELLOW boot state.\n");
			break;
		case RED:
			dprintf(INFO, "boot_verifier: Device is in RED boot state.\n");
			break;
	}
}

bool boot_verify_validate_keystore(unsigned char * user_addr)
{
	bool ret = false;
	unsigned char *input = user_addr;
	KEYSTORE *ks = NULL;
	uint32_t len = read_der_message_length(input);
	if(!len)
	{
		dprintf(CRITICAL, "boot_verifier: keystore length is invalid.\n");
		return ret;
	}

	ks = d2i_KEYSTORE(NULL, (const unsigned char **)&input, len);
	if(ks != NULL)
	{
		ret = true;
	}
	return ret;
}

static bool check_list(const char **list, const char* entry)
{
	if(list == NULL || entry == NULL)
		return false;

	while(*list != NULL)
	{
		if(!strcmp(entry, *list))
			return true;

		list++;
	}

	return false;
}