# Copyright 2021 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
################################################################################

FROM gcr.io/oss-fuzz-base/base-builder
RUN apt-get update && apt install -y libtalloc-dev libkqueue-dev libunwind-dev

# OpenSSL 3.x — installed to /usr/local/ssl to isolate from system OpenSSL 1.1.x
ARG OPENSSL_VERSION=3.4.1
ARG OPENSSL_HASH=002a2d6b30b58bf4bea46c43bdd96365aaf8daa6c428782aa4feee06da197df3
RUN set -ex && curl -sL -O https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz && echo "${OPENSSL_HASH} openssl-${OPENSSL_VERSION}.tar.gz" | sha256sum -c && tar -xzf openssl-${OPENSSL_VERSION}.tar.gz && cd openssl-${OPENSSL_VERSION} && ./Configure linux-x86_64 no-shared no-module --static --prefix=/usr/local/ssl "$CFLAGS" && make build_generated && make libcrypto.a libssl.a && make install_dev

COPY . freeradius-server
RUN rm -rf freeradius-server/mayhem
COPY mayhem/build.sh $SRC
COPY mayhem/patch.diff $SRC
WORKDIR $SRC/freeradius-server

ENV FUZZING_LANGUAGE=c++ SANITIZER=address
RUN compile
