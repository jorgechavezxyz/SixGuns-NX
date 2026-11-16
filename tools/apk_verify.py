"""Checks that an APK is Gameloft's official, untouched Six Guns 2.9.9a (pure Python, no openssl/Java).

APK signature scheme v1 (JAR signing), which this APK uses:
  META-INF/CERT.RSA  PKCS#7 SignedData: the signer's certificate + an RSA signature over CERT.SF
  META-INF/CERT.SF   digest of MANIFEST.MF (and of each manifest section)
  META-INF/MANIFEST.MF  digest of every file in the APK
verify_apk() checks all three links: the certificate is Gameloft's (SHA-256 fingerprint), the RSA
signature over CERT.SF is valid (PKCS#1 v1.5, computed with Python integers), the manifest digest
matches, and every file's digest matches with no file left uncovered. A copied certificate cannot pass:
changing any file breaks its digest, and re-computing the digests breaks the RSA signature.
"""
import base64
import hashlib
import re
import zipfile

GAMELOFT_CERT_SHA256 = "2173a248e2b11f9760b9167f398b183c92816cf8fc74afd3c64b22eddf50481c"

_HASH_OIDS = {  # digest algorithm OIDs (DER contents) -> hashlib constructor
    bytes.fromhex("2b0e03021a"): hashlib.sha1,
    bytes.fromhex("608648016503040201"): hashlib.sha256,
    bytes.fromhex("608648016503040202"): hashlib.sha384,
    bytes.fromhex("608648016503040203"): hashlib.sha512,
}
_DIGESTINFO_PREFIX = {  # PKCS#1 v1.5 DigestInfo prefixes
    hashlib.sha1: bytes.fromhex("3021300906052b0e03021a05000414"),
    hashlib.sha256: bytes.fromhex("3031300d060960864801650304020105000420"),
    hashlib.sha384: bytes.fromhex("3041300d060960864801650304020205000430"),
    hashlib.sha512: bytes.fromhex("3051300d060960864801650304020305000440"),
}
_RSA_OID = bytes.fromhex("2a864886f70d010101")
_MSG_DIGEST_OID = bytes.fromhex("2a864886f70d010904")


class VerifyError(Exception):
    pass


def _tlv(b, i):
    """One DER element at b[i]: (tag, start of contents, end of contents)."""
    tag = b[i]
    i += 1
    n = b[i]
    i += 1
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(b[i:i + k], "big")
        i += k
    return tag, i, i + n


def _children(b, start, end):
    out = []
    i = start
    while i < end:
        tag, s, e = _tlv(b, i)
        out.append((tag, i, s, e))
        i = e
    return out


def _parse_pkcs7(der):
    """Returns (certificate DER, (n, e), digest constructor, signed attributes DER or None, signature)."""
    tag, s, e = _tlv(der, 0)  # ContentInfo
    ci = _children(der, s, e)
    _, _, s0, e0 = ci[1]  # [0] content
    tag, s, e = _tlv(der, s0)  # SignedData
    sd = _children(der, s, e)
    certs = signer = None
    for t, at, cs, ce in sd:
        if t == 0xA0:
            certs = (cs, ce)
        elif t == 0x31 and certs is not None:
            signer = (cs, ce)
    if not certs or not signer:
        raise VerifyError("no certificate or signer in the signature block")
    t, at, cs, ce = _children(der, *certs)[0]
    cert = der[at:ce]
    # certificate -> tbsCertificate -> subjectPublicKeyInfo (the sequence holding the RSA OID)
    tbs = _children(der, cs, ce)[0]
    pub = None
    for t, at2, s2, e2 in _children(der, tbs[2], tbs[3]):
        if t == 0x30:
            inner = _children(der, s2, e2)
            if inner and inner[0][0] == 0x30:
                alg = _children(der, inner[0][2], inner[0][3])
                if alg and alg[0][0] == 0x06 and der[alg[0][2]:alg[0][3]] == _RSA_OID:
                    bitstr = inner[1]
                    _, rs, re_ = _tlv(der, bitstr[2] + 1)  # skip the unused-bits byte
                    ne = _children(der, rs, re_)
                    n = int.from_bytes(der[ne[0][2]:ne[0][3]], "big")
                    ex = int.from_bytes(der[ne[1][2]:ne[1][3]], "big")
                    pub = (n, ex)
    if pub is None:
        raise VerifyError("the certificate has no RSA key")
    # SignerInfo
    si_t, si_at, si_s, si_e = _children(der, *signer)[0]
    parts = _children(der, si_s, si_e)
    digest = attrs = sig = None
    for t, at, s3, e3 in parts:
        if t == 0x30 and digest is None and at != parts[1][1]:
            sub = _children(der, s3, e3)
            if sub and sub[0][0] == 0x06 and der[sub[0][2]:sub[0][3]] in _HASH_OIDS:
                digest = _HASH_OIDS[der[sub[0][2]:sub[0][3]]]
        elif t == 0xA0:
            attrs = b"\x31" + der[at + 1:e3]  # signed attributes are signed as a SET
        elif t == 0x04:
            sig = der[s3:e3]
    if digest is None or sig is None:
        raise VerifyError("unsupported signature block")
    return cert, pub, digest, attrs, sig, (der, si_s, si_e)


def _rsa_ok(pub, sig, digest_fn, data):
    n, e = pub
    k = (n.bit_length() + 7) // 8
    em = pow(int.from_bytes(sig, "big"), e, n).to_bytes(k, "big")
    want = _DIGESTINFO_PREFIX[digest_fn] + digest_fn(data).digest()
    pad = k - len(want) - 3
    return pad >= 8 and em == b"\x00\x01" + b"\xff" * pad + b"\x00" + want


def verify_apk(path, engine_sha256=None):
    """Raises VerifyError unless the APK is signed by Gameloft and untouched. Returns a short report dict."""
    try:
        z = zipfile.ZipFile(path)
    except zipfile.BadZipFile:
        raise VerifyError("not an APK (not a zip file; a download page saved as .apk?)")
    names = z.namelist()
    blocks = [n for n in names if re.match(r"META-INF/[^/]+\.RSA$", n)]
    if not blocks:
        raise VerifyError("no RSA signature block (META-INF/*.RSA)")
    block = blocks[0]
    sf_name = block[:-4] + ".SF"
    if sf_name not in names or "META-INF/MANIFEST.MF" not in names:
        raise VerifyError("incomplete signature (missing .SF or MANIFEST.MF)")
    cert, pub, digest_fn, attrs, sig, _ = _parse_pkcs7(z.read(block))
    fp = hashlib.sha256(cert).hexdigest()
    if fp != GAMELOFT_CERT_SHA256:
        raise VerifyError("not signed by Gameloft (a repackaged / re-signed copy)")
    sf = z.read(sf_name)
    if attrs is not None:
        # with signed attributes the signature covers them; their message digest must be the .SF's
        md = None
        _, a_s, a_e = _tlv(attrs, 0)
        for t, at, s, e in _children(attrs, a_s, a_e):
            a = _children(attrs, s, e)
            if a and attrs[a[0][2]:a[0][3]] == _MSG_DIGEST_OID:
                vals = _children(attrs, a[1][2], a[1][3])
                md = attrs[vals[0][2]:vals[0][3]]
        if md != digest_fn(sf).digest():
            raise VerifyError("signature attributes do not match CERT.SF")
        signed = attrs
    else:
        signed = sf
    if not _rsa_ok(pub, sig, digest_fn, signed):
        raise VerifyError("Gameloft's signature does not match the file (tampered)")
    mf = z.read("META-INF/MANIFEST.MF")
    sft = sf.decode("latin-1")
    m = re.search(r"(SHA-256|SHA1)-Digest-Manifest: (\S+)", sft)
    hashes = {"SHA-256": hashlib.sha256, "SHA1": hashlib.sha1}
    if not m or base64.b64encode(hashes[m.group(1)](mf).digest()).decode() != m.group(2):
        raise VerifyError("the signed manifest digest does not match (tampered)")
    text = mf.decode("latin-1").replace("\r\n", "\n").replace("\n ", "")
    entries = {n: (a, d) for n, a, d in re.findall(r"Name: (.+)\n(SHA-256|SHA1)-Digest: (\S+)", text)}
    files = [n for n in names if not n.endswith("/") and not n.startswith("META-INF/")]
    for n in files:
        if n not in entries:
            raise VerifyError(f"{n} is not covered by the signature (added after signing)")
        a, d = entries[n]
        if base64.b64encode(hashes[a](z.read(n)).digest()).decode() != d:
            raise VerifyError(f"{n} was changed after signing")
    report = {"signer": "Gameloft", "files": len(files)}
    if engine_sha256:
        lib = "lib/arm64-v8a/libAndroid.so"
        if lib not in names:
            raise VerifyError("no arm64 engine (lib/arm64-v8a/libAndroid.so)")
        if hashlib.sha256(z.read(lib)).hexdigest() != engine_sha256:
            raise VerifyError("official APK, but a different game version than 2.9.9a (engine differs)")
        report["engine"] = "2.9.9a"
    return report


if __name__ == "__main__":
    import sys
    for p in sys.argv[1:]:
        try:
            print(p, "->", verify_apk(p))
        except VerifyError as ex:
            print(p, "-> REJECTED:", ex)
