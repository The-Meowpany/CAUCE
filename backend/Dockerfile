# The base image is a build ARG with no default, so a build that has not decided which
# exact image it is using cannot start at all.
#
# A floating tag is the quiet way an artifact stops being reproducible. `python:3.12-slim`
# is a moving target: the same Dockerfile builds different bytes on different days, and a
# central that was tested against one set of system libraries ships against another. There
# is no digest to pin here by default because a digest that nobody re-resolves is a
# correctness claim with no evidence behind it, so the caller has to supply a real one:
#
#   docker build --build-arg BASE_IMAGE=python:3.12-slim@sha256:<digest> -t cauce-central .
#
# `scripts/release-gate.ps1` fails a release whose Dockerfile does not take the base image
# this way, so an unpinned base cannot reach a tag.
ARG BASE_IMAGE
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