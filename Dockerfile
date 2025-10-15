
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    g++ \
    curl \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

COPY . /app
WORKDIR /app

RUN g++ test.cpp -o server -std=c++17 -Wall -Wextra

EXPOSE 8787

CMD ["./server"]
