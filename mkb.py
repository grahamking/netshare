out = open("payload-test.txt", "wt")
for i in range(1 << 20):
    x = str(i) + " "
    out.write(x * 10 + "\n")
out.close()
