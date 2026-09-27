"""python -m rghport.saves import-save --wii-save <folder> --out <folder> --bigfile <file>"""
import sys

from .cli import main

sys.exit(main())
