The Xe probe stick writes its reports here, three files per scan:
  xe-stick-XR-XXXXXXXX.tar.gz   the report: attach this file to the GitHub issue
  xe-stick-XR-XXXXXXXX.txt      a summary of it, to read first
  xe-stick-XR-XXXXXXXX.sha256   the SHA-256 of both (the .tar.gz holds SEAL.sha256 for every file inside)
xe-stick-state.txt remembers the steps that froze a computer, so that the next scan leaves them out. Delete it to try
every step again.
