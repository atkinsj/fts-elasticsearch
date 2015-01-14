#!/bin/env python
from __future__ import print_function
import time
import imaplib
import email

def insert_test_email(conn, box, message):
    val = conn.append(box,
                      '',
                      imaplib.Time2Internaldate(time.time()),
                      str(email.message_from_string(message)))

    # there is a nicer way to do this
    uid = val[1][0].split(' ')[2][:-1]

    return uid

def expunge_inserted_emails(conn, inserted_emails):
    # need to makr as deleted

    conn.expunge()

def cleanup(conn, inserted_emails):
    expunge_inserted_emails(conn, inserted_emails)

def test():
    source_box = "INBOX"
    dest_box = "bob"
    dest_created = False
    inserted_emails = []

    # testing done over non-ssl for now
    conn = imaplib.IMAP4('127.0.0.1', 143)
    conn.login("test", "test")

    # ensure our mailboxes exist
    if conn.select(dest_box)[0] == 'NO':
        conn.create(dest_box)
        dest_created = True

    # perform a basic TEXT search on both mailboxes to ensure
    # the indicies are built in ES
    print("testing a basic full-text search ... ", end="")
    conn.select(dest_box)
    conn.search(None, 'text', 'foobar')

    conn.select(source_box)
    conn.search(None, 'text', 'foobar')
    print("pass")

    # add a test e-mail
    print("inserting a test e-mail in to %s ... " % source_box, end="")
    uid = insert_test_email(conn, source_box, "abcdefghijklmnopqrstuv")
    inserted_emails.append(uid)
    print("pass")

    # sleep to let the index update
    time.sleep(1)

    # find that test e-mail in the body field
    print("finding the e-mail we just inserted ... ", end="")
    conn.select(source_box)
    results = conn.search(None, 'body', "abcdefghijklmnopqrstuv")

    if uid in results[1][0]:
        print("pass")
    else:
        print("FAIL")

    # test moving the e-mail
    

    conn.close()

    # clean-up if we created this mailbox
    if dest_created:
        conn.delete(dest_box)

    conn.select(source_box)
    cleanup(conn, inserted_emails)

    conn.logout()

if __name__ == "__main__":
    test();