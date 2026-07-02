
## Getting started

To make it easy for you to get started with Rrapberry Pi, here's a list of recommended next steps.

## raspberry pi build&debug

sudo apt update

sudo apt install -y cmake g++ libcurl4-openssl-dev libssl-dev libspdlog-dev nlohmann-json3-dev zlib1g-dev

git clone v-wise-agent-cpp-sdk
cd v-wise-agent-cpp-sdk
mkdir build && cd build
cmake ..
make
