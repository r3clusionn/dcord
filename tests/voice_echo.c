/* voice_echo PORT KEYHEX: a stand-in for a Discord voice UDP server.
 * Answers IP discovery, decrypts aead_aes256_gcm_rtpsize packets with KEY and
 * sends each one back re-encrypted as SSRC 2222 (as if another user spoke). */
#include <arpa/inet.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

static unsigned char key[32];

static int aead(int enc, const unsigned char *nonce, const unsigned char *aad, int al, const unsigned char *in, int il,
		unsigned char *out, unsigned char *tag)
{
	EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
	int len, ok = 1;
	if (enc) {
		EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), NULL, key, nonce);
		EVP_EncryptUpdate(c, NULL, &len, aad, al);
		EVP_EncryptUpdate(c, out, &len, in, il);
		EVP_EncryptFinal_ex(c, out + len, &len);
		EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_GET_TAG, 16, tag);
	} else {
		EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), NULL, key, nonce);
		EVP_DecryptUpdate(c, NULL, &len, aad, al);
		EVP_DecryptUpdate(c, out, &len, in, il);
		EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_AEAD_SET_TAG, 16, (void *)tag);
		ok = EVP_DecryptFinal_ex(c, out + len, &len) == 1;
	}
	EVP_CIPHER_CTX_free(c);
	return ok;
}

int main(int argc, char **argv)
{
	if (argc < 3 || strlen(argv[2]) != 64)
		return fprintf(stderr, "usage: voice_echo PORT KEYHEX [--sfu]\n"), 2;
	int sfu = argc > 3 && !strcmp(argv[3], "--sfu");
	struct sockaddr_in peers[8];
	unsigned peer_ssrc[8];
	int npeers = 0;
	for (int i = 0; i < 32; i++)
		sscanf(argv[2] + 2 * i, "%2hhx", &key[i]);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(atoi(argv[1])), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
	if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0)
		return perror("bind"), 1;
	unsigned long good = 0, bad = 0, keep = 0;
	unsigned short seq = 0;
	unsigned ts = 0, ctr = 0;
	for (;;) {
		unsigned char b[2048], pt[2048], out[2048];
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		int n = recvfrom(fd, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
		if (n == 74 && b[1] == 1) {  /* IP discovery */
			unsigned char r[74] = {0, 2, 0, 70};
			memcpy(r + 4, b + 4, 4);
			strcpy((char *)r + 8, inet_ntoa(from.sin_addr));
			r[72] = ntohs(from.sin_port) >> 8;
			r[73] = ntohs(from.sin_port) & 0xff;
			sendto(fd, r, 74, 0, (struct sockaddr *)&from, fl);
			printf("echo: ip discovery from port %d\n", ntohs(from.sin_port));
			fflush(stdout);
			continue;
		}
		if (n == 8) {
			keep++;
			continue;
		}
		if (n < 12 + 20 || (b[0] >> 6) != 2)
			continue;
		int hl = 12 + 4 * (b[0] & 15);
		unsigned char nonce[12] = {0};
		memcpy(nonce, b + n - 4, 4);
		int cl = n - hl - 20;
		if (!aead(0, nonce, b, hl, b + hl, cl, pt, b + n - 20)) {
			bad++;
			printf("echo: DECRYPT FAILED (%lu)\n", bad);
			fflush(stdout);
			continue;
		}
		good++;
		if (sfu) {
			/* forward the packet unchanged to every other client (same key for all in the mock) */
			unsigned ssrc = (unsigned)b[8] << 24 | b[9] << 16 | b[10] << 8 | b[11];
			int found = 0;
			for (int i = 0; i < npeers; i++)
				if (peer_ssrc[i] == ssrc) {
					peers[i] = from;
					found = 1;
				}
			if (!found && npeers < 8) {
				peer_ssrc[npeers] = ssrc;
				peers[npeers++] = from;
			}
			for (int i = 0; i < npeers; i++)
				if (peer_ssrc[i] != ssrc)
					sendto(fd, b, n, 0, (struct sockaddr *)&peers[i], sizeof peers[i]);
			if (good % 100 == 1) {
				printf("sfu: %lu packets ok, %lu bad, %d clients, last payload %d bytes\n", good, bad, npeers, cl);
				fflush(stdout);
			}
			continue;
		}
		/* re-send as ssrc 2222 */
		unsigned char h[12] = {0x80, 0x78, seq >> 8, seq & 0xff, ts >> 24, ts >> 16, ts >> 8, ts, 0, 0, 0x08, 0xae};
		seq++;
		ts += 960;
		unsigned char n4[4] = {ctr >> 24, ctr >> 16, ctr >> 8, ctr};
		ctr++;
		unsigned char nn[12] = {0};
		memcpy(nn, n4, 4);
		memcpy(out, h, 12);
		aead(1, nn, h, 12, pt, cl, out + 12, out + 12 + cl);
		memcpy(out + 12 + cl + 16, n4, 4);
		sendto(fd, out, 12 + cl + 20, 0, (struct sockaddr *)&from, fl);
		if (good % 100 == 1) {
			printf("echo: %lu packets ok, %lu bad, %lu keepalives, last payload %d bytes\n", good, bad, keep, cl);
			fflush(stdout);
		}
	}
}
