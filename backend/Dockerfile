# The base image is a build ARG with no default, so a build that has not decided which
# exact image it is using cannot start at all.
#
# A floating tag is the quiet way an artifact stops being reproducible. `python:3.12-slim`
# is a moving target: the same Dockerfile builds different bytes on different days, and a
# central that was tested against one set of system libraries ships against another.
#
# The caller supplies a resolved digest, and the value below is the multi-platform index
# digest for `python:3.12-slim` at the time of writing, taken from the registry rather than
# composed from memory:
#
#   docker build --build-arg BASE_IMAGE=python:3.12-slim@sha256:02108f5d322dd89f1c9e552442c25acb0543dfdbc455693a5599624f20d9155d -t cauce-central .
#
# An index digest, not a per-architecture one, so the same value builds on amd64 and arm64 -
# which matters because the pilot Raspberry Pis are arm64 and an amd64 manifest digest would
# fail there rather than fall back. It is recorded in `scripts/release-gate.ps1`, which fails
# a release whose Dockerfile and gate disagree about it, so the two cannot drift.
#
# To move to a newer base, re-resolve it rather than editing the tag by hand:
#   docker buildx imagetools inspect python:3.12-slim --format '{{.Manifest.Digest}}'
ARG BASE_IMAGE=python:3.12-slim@sha256:02108f5d322dd89f1c9e552442c25acb0543dfdbc455693a5599624f20d9155d
FROM ${BASE_IMAGE}

WORKDIR /app

# requirements.txt is the loose, human-edited statement of what the project needs.
# requirements.lock is the pinned closure, with a SHA-256 for every artifact.
#
# This line used to install requirements.txt, which meant the pinned lock in the repository
# was documentation: the image that actually deployed resolved fresh versions of every
# dependency at build time, so two builds of the same commit produced different runtimes.
# A lock that the artifact does not install is worse than no lock, because the repository
# looks reproducible to anyone reading it.
COPY requirements.txt requirements.lock ./
RUN pip install --no-cache-dir --require-hashes -r requirements.lock

COPY cauce_server ./cauce_server

ENV CAUCE_DB_PATH=/data/cauce.sqlite
VOLUME ["/data"]
EXPOSE 8000

CMD ["uvicorn", "cauce_server.main:app", "--host", "0.0.0.0", "--port", "8000"]