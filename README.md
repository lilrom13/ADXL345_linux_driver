## Romain MARGHERITI MSSE 2018/2019

### Explorons le noyau Linux

1. *D'où proviennent les fichiers /boot/config-\* ? Dans quels cas sont-ils utiles ?*
- Les fichier config-\* sont générés par la commande *make config*.
- Ils sont utiles dans le cas ou nous souhaitons adapter le noyau aux besoins de l'utilisateur en renseignant des options particulières.
2. *Calculez la taille de chacun des répertoires à la racine (indice : utilisez la commande du). Qu'en pensez-vous ?*
- Le dossier le plus conséquent est le dossier *drivers* ce qui veut dire que le noyau est essentiellement composé de périphériques.

### Écriture d'un premier module
1. Quels sont les avantages de l'utilisation d'un module plutôt que du code compilé en dur directement dans l'image du noyau ?
2. Chargez votre module, pourquoi ne se passe-t-il rien (en apparence) ?
- En apparence il ne se passe rien car les messages sont stockés dans un tampon circulaire. Le niveau de priorité est trop bas pour être affiché dans la console.
3. Lisez la documentation et expliquez comment fonctionne le Makefile présenté plus haut.
- Le Makefile présenté plus haut commence par vérifier que la variable *KERNELRELEASE* ne vaut rien afin d'exécuter la condition *else*. Cette condition va appeler le *Makefile* du noyau en lui passant le chemin du répertoire courant. La variable *KERNELRELEASE* vaut cette fois ci quelque chose car nous utilisons le *Makefile* du noyau. Nous compilon alors notre objet *first.o* en tant que module (.ko).
