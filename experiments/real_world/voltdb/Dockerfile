FROM docker.io/library/openjdk:8-jdk
RUN apt-get update && apt-get install -y --no-install-recommends \
    ant cmake g++ gcc git make python3 wget curl pkg-config autoconf automake libtool \
    && rm -rf /var/lib/apt/lists/*
RUN git clone https://github.com/DBOS-project/voltdb.git /tmp/voltdb-src \
    && cd /tmp/voltdb-src \
    && git checkout 546ac588120fae801212ccf77c2e253a664d6b33 \
    && ant clean && ant \
    && mv /tmp/voltdb-src /opt/voltdb
ENV VOLTDB_HOME=/opt/voltdb
ENV PATH=/opt/voltdb/bin:$PATH
RUN mkdir -p /var/voltdb
WORKDIR /var/voltdb
RUN voltdb init --force
EXPOSE 21212 3021 5555 8080
CMD ["voltdb", "start", "-H", "localhost"]
