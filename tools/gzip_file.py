"""Gzip a file for embedding in the firmware: gzip_file.py <in> <out>"""
import gzip
import sys

with open(sys.argv[1], 'rb') as src:
    data = src.read()
with open(sys.argv[2], 'wb') as dst:
    # mtime=0 keeps the output byte-identical between builds
    dst.write(gzip.compress(data, compresslevel=9, mtime=0))
