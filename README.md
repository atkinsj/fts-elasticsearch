h1. fts-elasticsearch
fts-elasticsearch is a Dovecot full-text search indexing plugin that uses ElasticSearch as a backend.

Dovecot communicates to ES using HTTP/JSON queries.

*Experimental:*
This plugin is currently highly experimental and under active development. Expect changes, bugs, breaks.

h3. Requirements
* Minimum Dovecot version has not yet been determined
* ElasticSearch 1.0+
* JSON-C

h3. Compilation
This plugin needs to compile against the Dovecot source for the version you intend to run it on.

h4. Installation
A simple make install is sufficient. 