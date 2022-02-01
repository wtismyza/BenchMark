from distutils.core import setup

try:
    with open("README.rst") as f:
        long_desc = f.read()
except:
    long_desc = ""

setup(name="foundationdb",
      version="${FDB_VERSION}",
      author="FoundationDB",
      author_email="fdb-dist@apple.com",
      description="Python bindings for the FoundationDB database",
      url="https://www.foundationdb.org",
      packages=['fdb'],
      package_data={'fdb': ["fdb/*.py"]},
      long_description=long_desc,
      classifiers=[
          'Development Status :: 5 - Production/Stable',
          'Intended Audience :: Developers',
          'License :: OSI Approved :: Apache Software License',
          'Operating System :: MacOS :: MacOS X',
          'Operating System :: Microsoft :: Windows',
          'Operating System :: POSIX :: Linux',
          'Programming Language :: Python :: 2',
          'Programming Language :: Python :: 2.6',
          'Programming Language :: Python :: 2.7',
          'Programming Language :: Python :: 3',
          'Programming Language :: Python :: 3.0',
          'Programming Language :: Python :: 3.1',
          'Programming Language :: Python :: 3.2',
          'Programming Language :: Python :: 3.3',
          'Programming Language :: Python :: 3.4',
          'Programming Language :: Python :: Implementation :: CPython',
          'Topic :: Database',
          'Topic :: Database :: Front-Ends'
      ]
      )
