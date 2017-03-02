# Istio Proxy

This is server side branch, and client side branch is located at bug_131. 

Here is the step to run the end2end test, refer
https://screenshot.googleplex.com/sBJyhhV4Hqd to run mixer and app.

And bazel build //src/envoy/mixer:envoy to build envoy at both server/client
sides.

/bin/bash src/envoy/mixer/start_envoy -l debug to run envoy at both
server/client side (of course with different setup). 

From another tap, just 
curl http://localhost:9010/echo -d "hello world"

and you will see the work flow.

And if you want curl to talk to server side directly with mTLS, do this in
client side directory (which has the proper key and cert).

curl --cert client.cert.pem --key client.key.pem -v --cacert server-ca.cert.pem
https://localhost:9090/echo -d "wattllii"

