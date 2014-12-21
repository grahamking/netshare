FROM debian:stable
MAINTAINER Graham King <graham@gkgk.org>
EXPOSE 7999
COPY netshare /
COPY header.jpg /
ENTRYPOINT ["/netshare"]
CMD ["-p", "7999", "-m", "image/jpeg", "-h", "localhost", "/header.jpg"]
