#Check simple word
true

#check fail
jibjab

#Check colons
#: : :

#Test simple commands
echo echo1

#Test pipe
echo echo4 | cat

#Test simple commands with input-output
echo wrote to file > f1.txt
cat f1.txt
echo wrote to file again > f1.txt
cat < f1.txt

#Test pipe and sequence commands
echo echo5 | cat ; echo echo6; echo echo7

#Test if statement
if gobble
then
echo count
else
echo no count
fi

#Test if statement with input-output
if :
then
echo ball > test.txt
else
echo no ball > test.txt
fi

#test long command
#sort /usr/shar/dict/words > /dev/null

#More if statement tests
echo hello > in.txt
if cat; then cat; fi < in.txt

#Check while loops
touch file
touch file2
echo a > file2
cp file2 file
while cat file; do rm file; done

#Check above worked
echo notfailedabove

#Check until loops
touch file
touch file2
echo b > file2
cp file2 file
until cat file; do rm file; done

#Check subshells
(echo a)
(echo a | cat)
(echo bob) > file3
(exec echo bob)

#Clean all temp files
rm test.txt
rm file2
rm f1.txt
rm in.txt
rm file3
rm file

#Test exec command
#exec cat dog

#Test what happens after exec
echo exec_ran
