#! /usr/bin/perl -w

open(FOO, "ospfsmod.c") || die "Did you delete ospfsmod.c?";
$lines = 0;
$lines++ while defined($_ = <FOO>);
close FOO;

@tests = (
    # test reading 
    # 1
    [ 'diff base/hello.txt test/hello.txt >/dev/null 2>&1 && echo $?',
      "0"
    ],
    # 2
    [ 'cmp base/pokercats.gif test/pokercats.gif >/dev/null 2>&1 && echo $?',
      "0"
    ],
    # 3
    [ 'ls -l test/pokercats.gif | awk "{ print \$5 }"',
      "91308"
    ],

    # test writing
    # We use dd to write because it doesn't initially truncate, and it can
    # be told to seek forward to a particular point in the disk.
    # 4
    [ "echo Bybye | dd bs=1 count=5 of=test/hello.txt conv=notrunc >/dev/null 2>&1 ; cat test/hello.txt",
      "Bybye, world!"
    ],
    # 5
    [ "echo Hello | dd bs=1 count=5 of=test/hello.txt conv=notrunc >/dev/null 2>&1 ; cat test/hello.txt",
      "Hello, world!"
    ],
    # 6
    [ "echo gi | dd bs=1 count=2 seek=7 of=test/hello.txt conv=notrunc >/dev/null 2>&1 ; cat test/hello.txt",
      "Hello, girld!"
    ],
    # 7
    [ "echo worlds galore | dd bs=1 count=13 seek=7 of=test/hello.txt conv=notrunc >/dev/null 2>&1 ; cat test/hello.txt",
      "Hello, worlds galore"
    ],
    # 8
    [ "echo 'Hello, world!' > test/hello.txt ; cat test/hello.txt",
      "Hello, world!"
    ],
    
    # create a file
    # 9
    [ 'touch test/file1 && echo $?',
      "0"
    ],

    # read directory
    # 10
    [ 'touch test/dir-contents.txt ; ls test | tee test/dir-contents.txt | grep file1',
      'file1'
    ],

    # write files, remove them, then read dir again
    # 11
    [ 'ls test | dd bs=1 of=test/dir-contents.txt >/dev/null 2>&1; ' .
      ' touch test/foo test/bar test/baz && '.
      ' rm    test/foo test/bar test/baz && '.
      'diff <( ls test ) test/dir-contents.txt',
      ''
    ],

    # remove the last file
    # 12
    [ 'rm -f test/dir-contents.txt && ls test | grep dir-contents.txt',
      ''
    ],


    # write to a file
    # 13
    [ 'echo hello > test/file1 && cat test/file1',
      'hello'
    ],
    
    # append to a file
    # 14
    [ 'echo hello > test/file1 ; echo goodbye >> test/file1 && cat test/file1',
      'hello goodbye'
    ],

    # delete a file
    # 15
    [ 'rm -f test/file1 && ls test | grep file1',
      ''
    ],

    # make a larger file for indirect blocks
    # 16
    [ 'yes | head -n 5632 > test/yes.txt && ls -l test/yes.txt | awk \'{ print $5 }\'',
      '11264'
    ],
   
    # truncate the large file
    # 17
    [ 'echo truncernated11 > test/yes.txt | ls -l test/yes.txt | awk \'{ print $5 }\' ; rm test/yes.txt',
      '15'
    ],
    
    # create a symbolic link
    # 18
    [ 'echo this is foo > test/foo; ' .
      'ln -s foo test/sym && ' .
      'diff test/sym test/foo && ' .
      'rm test/sym test/foo',
      ''
    ],

    # create a symbolic link and change the file it points to
    # 19
    [ 'echo this is foo > test/foo; ' .
      'ln -s foo test/sym && ' .
      'rm test/foo && ' .
      'echo this is now bar > test/foo && ' .
      'cat test/sym && rm test/sym test/foo' ,
      'this is now bar'
    ],

    # check that a symlink is being interpreted relative to the working path
    # 20
    [ 'echo this is foo > test/foo; ' .
      'ln -s foo test/sym && ' .
      'ln -s message.txt test/subdir/sym && ' .
      'diff <( cat test/foo ) test/sym && ' .
      'diff <( cat test/subdir/message.txt) test/subdir/sym; ' .
      'cd test/subdir; ' .
      'diff <( cat ../foo ) ../sym && ' .
      'diff <( cat message.txt) sym; ' .
      'cd ../..; ' .
      'rm test/sym test/subdir/sym test/foo',
      ''
    ],

    # another check that symlinks work relative to current path
    # 21
    [ 'echo this is foo > test/foo; ' .
      'ln -s ../foo test/subdir/sym && ' .
      'diff test/foo test/subdir/sym && ' .
      'rm test/subdir/sym test/foo',
      ''
    ],

    # check conditional symlnk
    # 22
    [ 'cd test; '.
      'echo Root > root.txt; ' .
      'echo Not root > not_root.txt; ' .
      'chmod 777 not_root.txt; ' .
      'ln -s root?root.txt:not_root.txt amiroot && ' .
      'cat amiroot && ' .
      'su user -c "cat amiroot"; ' .
      'rm root.txt not_root.txt amiroot; ' .
      'cd ..',
      "Root Not root"
    ]

    
);

my($ntest) = 0;
my(@wanttests);

foreach $i (@ARGV) {
    $wanttests[$i] = 1 if (int($i) == $i && $i > 0 && $i <= @tests);
}

my($sh) = "bash";
my($tempfile) = "lab3test.txt";
my($ntestfailed) = 0;
my($ntestdone) = 0;

foreach $test (@tests) {
    $ntest++;
    next if (@wanttests && !$wanttests[$ntest]);
    $ntestdone++;
    print STDOUT "Running test $ntest\n";
    my($in, $want) = @$test;
    open(F, ">$tempfile") || die;
    print F $in, "\n";
    print STDERR "  ", $in, "\n";
    close(F);
    $result = `$sh < $tempfile 2>&1`;
    $result =~ s|\[\d+\]||g;
    $result =~ s|^\s+||g;
    $result =~ s|\s+| |g;
    $result =~ s|\s+$||;

    next if $result eq $want;
    next if $want eq 'Syntax error [NULL]' && $result eq '[NULL]';
    next if $result eq $want;
    print STDERR "Test $ntest FAILED!\n  input was \"$in\"\n  expected output like \"$want\"\n  got \"$result\"\n";
    $ntestfailed += 1;
}

unlink($tempfile);
my($ntestpassed) = $ntestdone - $ntestfailed;
print "$ntestpassed of $ntestdone tests passed\n";
exit(0);
