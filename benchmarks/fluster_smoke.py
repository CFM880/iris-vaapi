#!/usr/bin/env python3
"""Compatibility entry point; defaults to the three-path Fluster smoke suite."""
import sys
from run_fluster import main

if __name__ == "__main__":
    sys.exit(main())
