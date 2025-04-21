openssl req -x509 \
  -newkey rsa:2048 \
  -nodes \
  -keyout server.key \
  -out server.crt \
  -days 36500 \
  -config ssl.conf \
  -extensions v3_req