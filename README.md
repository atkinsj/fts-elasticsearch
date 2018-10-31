# fts-elasticsearch
fts-elasticsearch is a Dovecot full-text search indexing plugin that uses ElasticSearch as a backend.

Dovecot communicates to ES using HTTP/JSON queries. It supports automatic indexing and searching of e-mail.

## Requirements
* Dovecot 2.2+
* JSON-C
* ElasticSearch 6.x for your server
* Autoconf 2.53+

NOTE: You must add `-Des.xcontent.strict_duplicate_detection=false` to your Elasticsearch startup parameters. ES 7.0 will NOT be compatible with this plugin for now since this option will be deprecated.

## Compiling
This plugin needs to compile against the Dovecot source for the version you intend to run it on. A dovecot-devel package is unfortunately insufficient as it does not include the required fts API header files. 

You can provide the path to your source tree by passing --with-dovecot= to ./configure.

An example build may look like:

    ./autogen.sh
    ./configure --with-dovecot=/path/to/dovecot/src/root
    make
    make install

## Configuration
In dovecot/conf.d/10-mail.conf:

	mail_plugins = fts fts_elasticsearch

In dovecot/conf.d/90-plugins.conf:

	plugin {
	  fts = elasticsearch
	  fts_elasticsearch = debug url=http://localhost:9200/
	  fts_autoindex = yes
	}

There are only two supported configuration parameters at the moment:
* url=\<elasticsearch url\> Required base URL
* debug Enables HTTP debugging

## ElasticSearch Indicies
fts-elasticsearch creates an index per mail box with a hardcoded type of 'mail'. It creates one field for each field in the e-mail header and for the body.

An example of pushed data for a basic e-mail with no attachments:

	{
		"uid": 3,
		"box": "f40efa2f8f44ad54424000006e8130ae",
		"Return-Path": "<josh@localhost.localdomain>",
		"X-Original-To": "josh",
		"Delivered-To": "josh@localhost.localdomain",
		"Received": "by localhost.localdomain (Postfix, from userid 1000) id 07DA3140314; Thu,  8 Jan 2015 00:20:05 +1100 (AEDT)",
		"Date": "Thu, 08 Jan 2015 00:20:05 +1100",
		"To": "josh@localhost.localdomain",
		"Subject": "Test #3",
		"User-Agent": "Heirloom mailx 12.5 7\/5\/10",
		"MIME-Version": "1.0",
		"Content-Type": "text\/plain; charset=us-ascii",
		"Content-Transfer-Encoding": "7bit",
		"Message-Id": "<20150107132005.07DA3140314@localhost.localdomain>",
		"From": "josh <josh@localhost.localdomain>",
		"body": "This is the body of test #3.\n"
	}

An example search:

	{
	  "query": {
	    "query_string": {
	      "query": "*Dovecot*",
	      "fields": [
	        "subject"
	      ]
	    }
	  },
	  "fields": [
	    "uid",
	    "box"
	  ]
	}

## TODO
There are a number of things left to be implemented:
* Rescan
* Optimisation (if any)
* Multiple mailbox lookup (for clients that call lookup_multi; need to find one)

## Background
Dovecot currently supports two primary full text search indexing plugins `fts-solr` and `fts-squat`. I wanted to use an FTS service that I already had set-up and that wasn't quite as heavy as Solr, primarily to consolidate infrastructure and to focus resources on optimising our ES instances.

## Thanks
This plugin borrows heavily from dovecot itself particularly for the automatic detection of dovecont-config (see m4/dovecot.m4). The fts-solr and fts-squat plugins were also used as reference material for understanding the Dovecot FTS API.
