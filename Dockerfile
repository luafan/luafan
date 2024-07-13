FROM ubuntu:22.04

WORKDIR /root/
COPY ./ /root/
RUN bash build_docker_ubuntu.sh
