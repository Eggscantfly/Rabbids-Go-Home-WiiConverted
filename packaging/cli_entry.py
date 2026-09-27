"""Entry point of rghport-cli.exe: the rghport command line (the desktop app runs its commands through it)."""
import sys


def main() -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
        except (AttributeError, ValueError):
            pass
    from rghport.cli import main as cli_main
    return cli_main()


if __name__ == "__main__":
    sys.exit(main())
