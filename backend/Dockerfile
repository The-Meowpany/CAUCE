FROM python:3.12-slim

WORKDIR /app
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY cauce_server ./cauce_server

ENV CAUCE_DB_PATH=/data/cauce.sqlite
VOLUME ["/data"]
EXPOSE 8000

CMD ["uvicorn", "cauce_server.main:app", "--host", "0.0.0.0", "--port", "8000"]
