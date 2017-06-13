/*
* Wire
* Copyright (C) 2016 Wire Swiss GmbH
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/
#include <re.h>
#include <avs.h>
#include <gtest/gtest.h>
#include "fakes.hpp"
#include "ztest.h"


/*
 * Test-cases that involve only 1 instance of "struct mediaflow"
 */


static struct vidcodec dummy_vp8 = {
	.le        = LE_INIT,
	.pt        = "110",
	.name      = "VP8",
	.has_rtp   = true,
};


class TestMedia : public ::testing::Test {

public:
	virtual void SetUp() override
	{
		struct sa laddr;
		int err;

#if 0
		log_set_min_level(LOG_LEVEL_WARN);
		log_enable_stderr(true);
#endif

		err = audummy_init(&aucodecl);
		ASSERT_EQ(0, err);

		vidcodec_register(&vidcodecl, &dummy_vp8);

		sa_set_str(&laddr, "127.0.0.1", 0);

		err = create_dtls_srtp_context(&dtls, TLS_KEYTYPE_EC);
		ASSERT_EQ(0, err);

		err = mediaflow_alloc(&mf, dtls, &aucodecl, &laddr,
				      CRYPTO_DTLS_SRTP,
				      mediaflow_estab_handler,
				      mediaflow_close_handler,
				      this);
		ASSERT_EQ(0, err);
		ASSERT_TRUE(mf != NULL);

		mediaflow_set_gather_handler(mf, mediaflow_gather_handler);
	}

	virtual void TearDown() override
	{
		mem_deref(mf);
		mem_deref(dtls);
		audummy_close();
		vidcodec_unregister(&dummy_vp8);
	}

	static void mediaflow_gather_handler(void *arg)
	{
		TestMedia *tm = static_cast<TestMedia *>(arg);

		++tm->n_gather;

		re_cancel();
	}

	static void mediaflow_estab_handler(const char *crypto,
					    const char *codec,
					    const char *type,
					    const struct sa *sa,
					    void *arg)
	{
		TestMedia *tm = static_cast<TestMedia *>(arg);
		++tm->n_estab;
	}

	static void mediaflow_close_handler(int err, void *arg)
	{
		TestMedia *tm = static_cast<TestMedia *>(arg);
		++tm->n_close;
	}

protected:
	struct mediaflow *mf = nullptr;
	struct tls *dtls = nullptr;
	struct list aucodecl = LIST_INIT;
	struct list vidcodecl = LIST_INIT;

	size_t candc_expected = 0;

	/* count how many times the callback handlers are called */
	unsigned n_estab = 0;
	unsigned n_close = 0;
	unsigned n_gather = 0;
};


static bool find_in_sdp(const char *sdp, const char *str)
{
	return 0 == re_regex(sdp, str_len(sdp), str);
}


TEST_F(TestMedia, alloc_and_not_ready)
{
	ASSERT_FALSE(mediaflow_is_ready(mf));
}


TEST_F(TestMedia, init)
{
	ASSERT_EQ(0, n_gather);
	ASSERT_EQ(0, n_estab);
	ASSERT_EQ(0, n_close);
}


TEST_F(TestMedia, sdp_offer_with_no_codecs)
{
	char sdp[4096];
	int err;

	err = mediaflow_generate_offer(mf, sdp, sizeof(sdp));
	ASSERT_EQ(0, err);

	ASSERT_EQ(0, n_gather);
	ASSERT_EQ(0, n_estab);
	ASSERT_EQ(0, n_close);

	/* simple verification of SDP offer */
	ASSERT_TRUE(strstr(sdp, "c=IN IP4 127.0.0.1"));
	ASSERT_TRUE(0 == re_regex(sdp, strlen(sdp),
				  "m=audio [0-9]+ UDP/TLS/RTP/SAVPF ", NULL));
}


TEST(media, ice_cand_decode)
{
	struct ice_cand_attr cand;
	struct sa addr;
	int err;

	err = ice_cand_attr_decode(&cand,
			      "42 1 udp 2113937151 10.0.0.63 2004 typ host");
	ASSERT_EQ(0, err);

	sa_set_str(&addr, "10.0.0.63", 2004);

	ASSERT_STREQ("42", cand.foundation);
	ASSERT_EQ(1, cand.compid);
	ASSERT_EQ(IPPROTO_UDP, cand.proto);
	ASSERT_EQ(2113937151, cand.prio);
	ASSERT_TRUE(sa_cmp(&addr, &cand.addr, SA_ALL));
	ASSERT_EQ(ICE_CAND_TYPE_HOST, cand.type);
}


TEST_F(TestMedia, gather_stun)
{
	StunServer srv;
	int err;

	candc_expected = 1;

	err = mediaflow_gather_stun(mf, &srv.addr);
	ASSERT_EQ(0, err);

	err = re_main_wait(5000);
	ASSERT_EQ(0, err);

	/* verify results after traffic is complete */
	ASSERT_TRUE(srv.nrecv > 0);
	ASSERT_EQ(1, n_gather);
}


TEST_F(TestMedia, gather_turn)
{
	TurnServer srv;
	int err;

	candc_expected = 2;

	err = mediaflow_gather_turn(mf, &srv.addr, "user", "pass");
	ASSERT_EQ(0, err);

	err = re_main_wait(10000);
	ASSERT_EQ(0, err);

	/* verify results after traffic is complete */
	ASSERT_TRUE(srv.nrecv > 0);
	ASSERT_EQ(1, n_gather);
}


TEST_F(TestMedia, chrome_interop)
{
	char answer[4096];
	int err;

	static const char *sdp_chrome =

"v=0\r\n"
"o=- 7592746549217333175 2 IN IP4 127.0.0.1\r\n"
"s=-\r\n"
"t=0 0\r\n"
"a=group:BUNDLE audio\r\n"
"a=msid-semantic: WMS 63CzX8x0XXu6h0EJXHVg1JVBdRTp954BPL6M\r\n"
"m=audio 1 RTP/SAVPF 111 103 104 0 8 106 105 13 126\r\n"
"c=IN IP4 0.0.0.0\r\n"
"a=rtcp:1 IN IP4 0.0.0.0\r\n"
"a=ice-ufrag:l7J3IU942KErkh/V\r\n"
"a=ice-pwd:oORc7rLRvan7Nf2A6c+QjRkn\r\n"
"a=ice-options:google-ice\r\n"
"a=fingerprint:sha-256 1D:A8:0B:46:EF:25:C9:3D:D1:D5:06:B9:9B:41:BE:DB:42:D6:15:D3:BA:C5:D5:99:FA:CC:92:74:AE:36:22:AB\r\n"
"a=setup:actpass\r\n"
"a=mid:audio\r\n"
"a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
"a=extmap:3 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
"a=sendrecv\r\n"
"a=rtcp-mux\r\n"
"a=rtpmap:111 opus/48000/2\r\n"
"a=fmtp:111 minptime=10\r\n"
"a=rtpmap:103 ISAC/16000\r\n"
"a=rtpmap:104 ISAC/32000\r\n"
"a=rtpmap:0 PCMU/8000\r\n"
"a=rtpmap:8 PCMA/8000\r\n"
"a=rtpmap:106 CN/32000\r\n"
"a=rtpmap:105 CN/16000\r\n"
"a=rtpmap:13 CN/8000\r\n"
"a=rtpmap:126 telephone-event/8000\r\n"
"a=maxptime:60\r\n"
"a=ssrc:267209345 cname:pKJMJctHTdncMCWy\r\n"
"a=ssrc:267209345 msid:63CzX8x0XXu6h0EJXHVg1JVBdRTp954BPL6M 477015ee-3ed3-44c2-95ef-9d4e1454638d\r\n"
"a=ssrc:267209345 mslabel:63CzX8x0XXu6h0EJXHVg1JVBdRTp954BPL6M\r\n"
"a=ssrc:267209345 label:477015ee-3ed3-44c2-95ef-9d4e1454638d\r\n"
		;

	err = mediaflow_offeranswer(mf, answer, sizeof(answer), sdp_chrome);
	ASSERT_EQ(0, err);

	ASSERT_EQ(CRYPTO_DTLS_SRTP, mediaflow_crypto(mf));

	ASSERT_TRUE(find_in_sdp(answer, "fingerprint:sha-256"));
	ASSERT_TRUE(find_in_sdp(answer, "rtcp-mux"));
	ASSERT_FALSE(find_in_sdp(answer, "setup:actpass"));
}


TEST_F(TestMedia, firefox_interop)
{
	char answer[4096];
	int err;

	static const char *sdp_firefox =

	"v=0\r\n"
	"o=Mozilla-SIPUA-31.0 27952 0 IN IP4 0.0.0.0\r\n"
	"s=SIP Call\r\n"
	"t=0 0\r\n"
	"a=ice-ufrag:c1b6b3f9\r\n"
	"a=ice-pwd:ee95ef6683918f54eb890b03cd9d0864\r\n"
	"a=fingerprint:sha-256"
	" 76:26:23:AB:46:FC:19:F3:78:45:84:F4:0A:2C:12:09"
	":70:97:4D:DD:BB:BB:B8:64:81:12:85:70:6E:27:3E:80\r\n"
	"m=audio 42496 RTP/SAVPF 109 0 8 101\r\n"
	"c=IN IP4 54.73.198.45\r\n"
	"a=rtpmap:109 opus/48000/2\r\n"
	"a=ptime:20\r\n"
	"a=rtpmap:0 PCMU/8000\r\n"
	"a=rtpmap:8 PCMA/8000\r\n"
	"a=rtpmap:101 telephone-event/8000\r\n"
	"a=fmtp:101 0-15\r\n"
	"a=sendrecv\r\n"
	"a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
	"a=setup:actpass\r\n"
	"a=candidate:0 1 UDP 2128609535 169.254.80.80 64005 typ host\r\n"
	"a=candidate:3 1 UDP 2128543999 192.168.10.181 64006 typ host\r\n"
	"a=candidate:4 1 UDP 1692401663 62.96.148.44 64006 typ srflx raddr"
	" 192.168.10.181 rport 64006\r\n"
	"a=candidate:5 1 UDP 98566143 54.73.198.45 42496 typ relay raddr"
	" 54.73.198.45 rport 42496\r\n"
	"a=candidate:0 2 UDP 2128609534 169.254.80.80 64007 typ host\r\n"
	"a=candidate:3 2 UDP 2128543998 192.168.10.181 64008 typ host\r\n"
	"a=candidate:4 2 UDP 1692401662 62.96.148.44 64008 typ srflx raddr"
	" 192.168.10.181 rport 64008\r\n"
	"a=candidate:5 2 UDP 98566142 54.73.198.45 44751 typ relay raddr"
	" 54.73.198.45 rport 44751\r\n"
	"a=rtcp-mux\r\n"
	;

	err = mediaflow_offeranswer(mf, answer, sizeof(answer), sdp_firefox);
	ASSERT_EQ(0, err);

	ASSERT_EQ(CRYPTO_DTLS_SRTP, mediaflow_crypto(mf));

	ASSERT_TRUE(find_in_sdp(answer, "fingerprint:sha-256"));
	ASSERT_TRUE(find_in_sdp(answer, "rtcp-mux"));
}


TEST_F(TestMedia, firefox38_interop)
{
	char answer[4096];
	int err;

	static const char *sdp_firefox =
"v=0\r\n"
"o=mozilla...THIS_IS_SDPARTA-38.0 6105375692410221769 0 IN IP4 0.0.0.0\r\n"
"s=-\r\n"
"t=0 0\r\n"
"a=fingerprint:sha-256 F3:61:9D:9B:88:24:C9:A5:2B:55:19:22:A4:E1:CA:DC:FA:8A:08:C1:A8:AE:3A:75:C3:CC:C2:22:F9:A2:94:D7\r\n"
"a=group:BUNDLE sdparta_0\r\n"
"a=ice-options:trickle\r\n"
"a=msid-semantic:WMS *\r\n"
"m=audio 9 RTP/SAVPF 109 9 0 8\r\n"
"c=IN IP4 0.0.0.0\r\n"
"a=sendrecv\r\n"
"a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
"a=ice-pwd:a7f4318106e9efc8c9678c3b59ca0fed\r\n"
"a=ice-ufrag:f4c7eb31\r\n"
"a=mid:sdparta_0\r\n"
"a=msid:{0557778a-a1d2-0f48-ace8-312ce8dd7ea4} {bb204ed0-0370-fc4c-bd6b-2d17d65d1c79}\r\n"
"a=rtcp-mux\r\n"
"a=rtpmap:109 opus/48000/2\r\n"
"a=rtpmap:9 G722/8000/1\r\n"
"a=rtpmap:0 PCMU/8000\r\n"
"a=rtpmap:8 PCMA/8000\r\n"
"a=setup:actpass\r\n"
"a=ssrc:982082819 cname:{ef5bfe23-4153-334d-bbea-395971ed2205}\r\n"
	;

	err = mediaflow_offeranswer(mf, answer, sizeof(answer), sdp_firefox);
	ASSERT_EQ(0, err);

	ASSERT_EQ(CRYPTO_DTLS_SRTP, mediaflow_crypto(mf));

	/* verify that we replace the mid value with the
	   incoming offer */
	ASSERT_TRUE(find_in_sdp(answer, "mid:sdparta_0"));
}


TEST_F(TestMedia, firefox45_interop)
{
	char answer[4096];
	struct sa laddr;
	int err;

	static const char *sdp_firefox =
"v=0\r\n"
"o=mozilla...THIS_IS_SDPARTA-45.0.2 7767043308804270395 0 IN IP4 0.0.0.0\r\n"
"s=-\r\n"
"t=0 0\r\n"
"a=sendrecv\r\n"
"a=fingerprint:sha-256 CD:7B:7C:A8:4F:A0:6D:DD:32:6F:3E:DD:F3:2D:07:F6:10:4D:D1:8F:E1:7F:95:32:7E:CB:33:17:BA:5B:65:19\r\n"
"a=group:BUNDLE audio video\r\n"
"a=ice-options:trickle\r\n"
"a=msid-semantic:WMS *\r\n"
"m=audio 50194 RTP/SAVPF 96\r\n"
"c=IN IP4 54.155.57.143\r\n"
"a=candidate:0 1 UDP 2122252543 192.168.10.88 60503 typ host\r\n"
"a=candidate:1 1 UDP 1686052863 62.96.148.44 60503 typ srflx raddr 192.168.10.88 rport 60503\r\n"
"a=candidate:2 1 UDP 92217343 54.155.57.143 50194 typ relay raddr 54.155.57.143 rport 50194\r\n"
"a=sendrecv\r\n"
"a=end-of-candidates\r\n"
"a=ice-pwd:0393917a2d22af7bd38e661130e77d41\r\n"
"a=ice-ufrag:cc92c585\r\n"
"a=mid:audio\r\n"
"a=msid:{da2198be-27c6-3844-871d-313e73fef45d} {d6ff7b2c-f689-a843-832c-843f59b52bbb}\r\n"
"a=rtcp-mux\r\n"
"a=rtpmap:96 opus/48000/2\r\n"
"a=setup:actpass\r\n"
"a=ssrc:2997989063 cname:{84d8043a-40ef-9b42-aff7-1104f80aaf43}\r\n"
"m=video 50194 RTP/SAVPF 100\r\n"
"c=IN IP4 54.155.57.143\r\n"
"a=recvonly\r\n"
"a=fmtp:100 max-fs=12288;max-fr=60\r\n"
"a=ice-pwd:0393917a2d22af7bd38e661130e77d41\r\n"
"a=ice-ufrag:cc92c585\r\n"
"a=mid:video\r\n"
"a=rtcp-fb:100 nack\r\n"
"a=rtcp-fb:100 nack pli\r\n"
"a=rtcp-fb:100 ccm fir\r\n"
"a=rtcp-mux\r\n"
"a=rtpmap:100 VP8/90000\r\n"
"a=setup:actpass\r\n"
"a=ssrc:934653567 cname:{84d8043a-40ef-9b42-aff7-1104f80aaf43}\r\n"
	;

	sa_set_str(&laddr, "127.0.0.1", 0);

	/* Populate only 1 ICE candidate (plus EOC) */
	err = mediaflow_add_local_host_candidate(mf, "eth0", &laddr);
	ASSERT_EQ(0, err);
	mediaflow_set_local_eoc(mf);

	err = mediaflow_add_video(mf, &vidcodecl);
	ASSERT_EQ(0, err);

	err = mediaflow_offeranswer(mf, answer, sizeof(answer), sdp_firefox);
	ASSERT_EQ(0, err);

	ASSERT_EQ(CRYPTO_DTLS_SRTP, mediaflow_crypto(mf));
	ASSERT_TRUE(mediaflow_have_eoc(mf));

#if 0
	re_printf("%s\n", answer);
#endif

	/* verify some SDP attributes */
	ASSERT_TRUE(find_in_sdp(answer, "a=fingerprint:sha-256"));
	ASSERT_TRUE(find_in_sdp(answer, "a=group:BUNDLE audio video"));
	ASSERT_TRUE(find_in_sdp(answer, "a=ice-options:trickle"));
	ASSERT_TRUE(find_in_sdp(answer, "a=end-of-candidates"));
	ASSERT_TRUE(find_in_sdp(answer, "a=ice-pwd"));
	ASSERT_TRUE(find_in_sdp(answer, "a=ice-ufrag"));
	ASSERT_TRUE(find_in_sdp(answer, "a=mid:audio"));
	ASSERT_TRUE(find_in_sdp(answer, "a=rtcp-mux"));
	ASSERT_TRUE(find_in_sdp(answer, "a=setup:active"));

	ASSERT_TRUE(find_in_sdp(answer, "m=video"));
	ASSERT_TRUE(find_in_sdp(answer, "a=mid:video"));
	ASSERT_TRUE(find_in_sdp(answer, "VP8/90000"));
}


TEST_F(TestMedia, verify_trickle_option_in_sdp)
{
	char sdp[4096];
	int err;

	err = mediaflow_generate_offer(mf, sdp, sizeof(sdp));
	ASSERT_EQ(0, err);

	ASSERT_TRUE(find_in_sdp(sdp, "trickle"));
}


TEST_F(TestMedia, verify_sha256_fingerprint_in_offer)
{
	char sdp[4096];
	struct pl pl;
	size_t i;
	int err;

	err = mediaflow_generate_offer(mf, sdp, sizeof(sdp));
	ASSERT_EQ(0, err);

	ASSERT_TRUE(find_in_sdp(sdp, "fingerprint:sha-256"));
	ASSERT_TRUE(find_in_sdp(sdp, "setup:actpass"));

	err = re_regex(sdp, strlen(sdp), "fingerprint:sha-256 [^\r\n]+", &pl);
	ASSERT_EQ(0, err);

	/* Firefox has a strict SDP parser, hex values MUST be uppercase! */
	for (i=0; i<pl.l; i++) {

		char c = pl.p[i];

		if (c == ':' ||
		    ('0' <= c && c <= '9') ||
		    ('A' <= c && c <= 'F'))
			continue;

		re_fprintf(stderr, "invalid character in fingerprint (%r)\n", &pl);
		ASSERT_TRUE(false);
	}
}


#if 0
TEST_F(TestMedia, verify_end_of_candidates)
{
	TurnServer srv;
	int err;

	candc_expected = 2;

	err = mediaflow_gather_turn(mf, &srv.addr, "user", "pass");
	ASSERT_EQ(0, err);

	err = re_main_wait(10000);
	ASSERT_EQ(0, err);

	/* verify results after traffic is complete */
	ASSERT_EQ(1, n_local_eoc);
}
#endif


TEST_F(TestMedia, sdp_offer_with_audio_only)
{
	char sdp[4096];
	int err;

	err = mediaflow_generate_offer(mf, sdp, sizeof(sdp));
	ASSERT_EQ(0, err);

	/* verify audio */
	ASSERT_TRUE(find_in_sdp(sdp, "m=audio"));
	ASSERT_TRUE(find_in_sdp(sdp, "a=mid:audio"));

	/* verify NOT video */
	ASSERT_FALSE(find_in_sdp(sdp, "m=video"));
	ASSERT_FALSE(find_in_sdp(sdp, "a=mid:video"));
	ASSERT_FALSE(find_in_sdp(sdp, "VP8/90000"));
}


TEST_F(TestMedia, sdp_offer_with_audio_and_video_codecs)
{
	char sdp[4096];
	int err;

	err = mediaflow_add_video(mf, &vidcodecl);
	ASSERT_EQ(0, err);

	err = mediaflow_generate_offer(mf, sdp, sizeof(sdp));
	ASSERT_EQ(0, err);

	/* verify session */
	ASSERT_TRUE(find_in_sdp(sdp, "a=group:BUNDLE audio video"));

	/* verify audio */
	ASSERT_TRUE(find_in_sdp(sdp, "m=audio"));
	ASSERT_TRUE(find_in_sdp(sdp, "a=mid:audio"));

	/* verify video */
	ASSERT_TRUE(find_in_sdp(sdp, "m=video"));
	ASSERT_TRUE(find_in_sdp(sdp, "a=mid:video"));
	ASSERT_TRUE(find_in_sdp(sdp, "VP8/90000"));


	// todo: video-line should contain ice-ufrag/pwd ?
	// todo: video-line should contain fingerprint/setup ?

	ASSERT_TRUE(find_in_sdp(sdp, "setup:actpass"));
}


TEST_F(TestMedia, interop_video_chrome46)
{
	char answer[4096];
	uint32_t ssrc, lssrc;
	char ssrc_buf[64];
	int err;

	static const char *sdp_offer =

"v=0\r\n"
"o=- 4100060250945197045 2 IN IP4 127.0.0.1\r\n"
"s=-\r\n"
"t=0 0\r\n"
"a=group:BUNDLE audio video\r\n"
"a=msid-semantic: WMS AGRoYYTpQWS3qv6fPyOWpt15gD99H4djXHCZ\r\n"
"m=audio 9 UDP/TLS/RTP/SAVPF 111 103 104 9 0 8 106 105 13 126\r\n"
"c=IN IP4 0.0.0.0\r\n"
"a=rtcp:9 IN IP4 0.0.0.0\r\n"
"a=ice-ufrag:4Tcbs3d0p+vKVwD+\r\n"
"a=ice-pwd:9wxCZUCAEWohFKxm2AVs9mH7\r\n"
"a=fingerprint:sha-256 41:2A:85:64:DE:9A:21:7B:42:61:95:82:D2:96:9B:9B:AD:02:7D:FD:00:B0:2E:37:C9:FF:A8:E4:4F:62:FA:A8\r\n"
"a=setup:actpass\r\n"
"a=mid:audio\r\n"
"a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n"
"a=extmap:3 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
"a=sendrecv\r\n"
"a=rtcp-mux\r\n"
"a=rtpmap:111 opus/48000/2\r\n"
"a=fmtp:111 minptime=10; useinbandfec=1\r\n"
"a=rtpmap:103 ISAC/16000\r\n"
"a=rtpmap:104 ISAC/32000\r\n"
"a=rtpmap:9 G722/8000\r\n"
"a=rtpmap:0 PCMU/8000\r\n"
"a=rtpmap:8 PCMA/8000\r\n"
"a=rtpmap:106 CN/32000\r\n"
"a=rtpmap:105 CN/16000\r\n"
"a=rtpmap:13 CN/8000\r\n"
"a=rtpmap:126 telephone-event/8000\r\n"
"a=maxptime:60\r\n"
"a=ssrc:3138421712 cname:+UCXQzJUFa7gOft1\r\n"
"a=ssrc:3138421712 msid:AGRoYYTpQWS3qv6fPyOWpt15gD99H4djXHCZ 4fdd486e-b8a9-4d2b-8f88-a0eede6b12ec\r\n"
"a=ssrc:3138421712 mslabel:AGRoYYTpQWS3qv6fPyOWpt15gD99H4djXHCZ\r\n"
"a=ssrc:3138421712 label:4fdd486e-b8a9-4d2b-8f88-a0eede6b12ec\r\n"
"m=video 9 UDP/TLS/RTP/SAVPF 100 116 117 96\r\n"
"c=IN IP4 0.0.0.0\r\n"
"a=rtcp:9 IN IP4 0.0.0.0\r\n"
"a=ice-ufrag:4Tcbs3d0p+vKVwD+\r\n"
"a=ice-pwd:9wxCZUCAEWohFKxm2AVs9mH7\r\n"
"a=fingerprint:sha-256 41:2A:85:64:DE:9A:21:7B:42:61:95:82:D2:96:9B:9B:AD:02:7D:FD:00:B0:2E:37:C9:FF:A8:E4:4F:62:FA:A8\r\n"
"a=setup:actpass\r\n"
"a=mid:video\r\n"
"a=extmap:2 urn:ietf:params:rtp-hdrext:toffset\r\n"
"a=extmap:3 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time\r\n"
"a=extmap:4 urn:3gpp:video-orientation\r\n"
"a=sendrecv\r\n"
"a=rtcp-mux\r\n"
"a=rtpmap:100 VP8/90000\r\n"
"a=rtcp-fb:100 ccm fir\r\n"
"a=rtcp-fb:100 nack\r\n"
"a=rtcp-fb:100 nack pli\r\n"
"a=rtcp-fb:100 goog-remb\r\n"
"a=rtpmap:116 red/90000\r\n"
"a=rtpmap:117 ulpfec/90000\r\n"
"a=rtpmap:96 rtx/90000\r\n"
"a=fmtp:96 apt=100\r\n"
"a=ssrc-group:FID 2086005321 1010864438\r\n"
"a=ssrc:2086005321 cname:+UCXQzJUFa7gOft1\r\n"
"a=ssrc:2086005321 msid:AGRoYYTpQWS3qv6fPyOWpt15gD99H4djXHCZ 27e3821b-eb50-47f7-9d0c-8b07d738ef31\r\n"
"a=ssrc:2086005321 mslabel:AGRoYYTpQWS3qv6fPyOWpt15gD99H4djXHCZ\r\n"
"a=ssrc:2086005321 label:27e3821b-eb50-47f7-9d0c-8b07d738ef31\r\n"
"a=ssrc:1010864438 cname:+UCXQzJUFa7gOft1\r\n"
"a=ssrc:1010864438 msid:AGRoYYTpQWS3qv6fPyOWpt15gD99H4djXHCZ 27e3821b-eb50-47f7-9d0c-8b07d738ef31\r\n"
"a=ssrc:1010864438 mslabel:AGRoYYTpQWS3qv6fPyOWpt15gD99H4djXHCZ\r\n"
"a=ssrc:1010864438 label:27e3821b-eb50-47f7-9d0c-8b07d738ef31\r\n"

		;

	err = mediaflow_add_video(mf, &vidcodecl);
	ASSERT_EQ(0, err);

	err = mediaflow_offeranswer(mf, answer, sizeof(answer), sdp_offer);
	ASSERT_EQ(0, err);

	ASSERT_EQ(CRYPTO_DTLS_SRTP, mediaflow_crypto(mf));
	ASSERT_TRUE(mediaflow_has_video(mf));

	/* verify bundle? */
	ASSERT_TRUE(find_in_sdp(answer, "a=group:BUNDLE audio video"));

	/* verify audio */
	ASSERT_FALSE(find_in_sdp(answer, "audio 0"));
	ASSERT_TRUE(find_in_sdp(answer, "a=sendrecv"));
	ASSERT_TRUE(find_in_sdp(answer, "a=rtcp-mux"));
	ASSERT_TRUE(find_in_sdp(answer, "a=ice-ufrag"));
	ASSERT_TRUE(find_in_sdp(answer, "a=ice-pwd"));
	ASSERT_TRUE(find_in_sdp(answer, "a=mid:audio"));
	ASSERT_TRUE(find_in_sdp(answer, "fingerprint:sha-256"));
	ASSERT_TRUE(find_in_sdp(answer, "a=setup:active"));

	re_snprintf(ssrc_buf, sizeof(ssrc_buf), "a=ssrc:%u",
		    mediaflow_get_local_ssrc(mf, MEDIA_AUDIO));
	ASSERT_TRUE(find_in_sdp(answer, ssrc_buf));

	err = mediaflow_get_remote_ssrc(mf, MEDIA_AUDIO, &ssrc);
	ASSERT_EQ(0, err);
	ASSERT_EQ(3138421712, ssrc);

	/* verify video */
	ASSERT_TRUE(find_in_sdp(answer, "m=video"));
	ASSERT_FALSE(find_in_sdp(answer, "video 0"));

	re_snprintf(ssrc_buf, sizeof(ssrc_buf), "a=ssrc:%u",
		    mediaflow_get_local_ssrc(mf, MEDIA_VIDEO));
	ASSERT_TRUE(find_in_sdp(answer, ssrc_buf));

	err = mediaflow_get_remote_ssrc(mf, MEDIA_VIDEO, &ssrc);
	ASSERT_EQ(0, err);
	ASSERT_EQ(2086005321, ssrc);

	ASSERT_TRUE(mediaflow_got_sdp(mf));
	ASSERT_TRUE(mediaflow_sdp_is_complete(mf));
}


TEST_F(TestMedia, sdp_offer_with_webrtc_rtp_profile)
{
	char sdp[4096];
	int err;

	err = mediaflow_add_video(mf, &vidcodecl);
	ASSERT_EQ(0, err);

	err = mediaflow_generate_offer(mf, sdp, sizeof(sdp));
	ASSERT_EQ(0, err);

	/* simple verification of SDP offer */
	ASSERT_EQ(0, re_regex(sdp, strlen(sdp),
			      "m=audio [0-9]+ UDP/TLS/RTP/SAVPF ", NULL));
	ASSERT_EQ(0, re_regex(sdp, strlen(sdp),
			      "m=video [0-9]+ UDP/TLS/RTP/SAVPF ", NULL));
}


TEST_F(TestMedia, sdp_offer_with_bandwidth_attr)
{
	char sdp[4096];
	int err;

	err = mediaflow_add_video(mf, &vidcodecl);
	ASSERT_EQ(0, err);

	err = mediaflow_generate_offer(mf, sdp, sizeof(sdp));
	ASSERT_EQ(0, err);

	/* verify audio */
	ASSERT_TRUE(find_in_sdp(sdp, "b=AS:50"));

	/* verify video */
	ASSERT_TRUE(find_in_sdp(sdp, "b=AS:800"));
}
